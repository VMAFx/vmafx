/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Imported frames on a HIP device (RC4 WP3, ADR-1852 design section 2.7,
 * ADR-1929, ADR-2092).
 *
 * Device pointers and dma-bufs (import_dmabuf.c) are bound where they are:
 * the planes of a planar frame are the producer's memory, with no
 * device-to-device copy into a pool. The HIP twins read a frame's planes with
 * copies or a conversion kernel enqueued on the device's library stream
 * (core/src/hip/picture_hip.h), which take any address and pitch, so no
 * layout is refused for alignment. NV12 / P010 / P016 are planarised on the
 * device into planes of the frame's own (import_convert.hip: a de-interleave
 * and the P010 shift, nothing else), so an imported frame scores bit for bit
 * as the same frame uploaded from the host. HIP arrays are not linear
 * memory: a semi-planar frame in arrays is converted the same way, a planar
 * one is copied on the device only with VMAFX_IMPORT_ALLOW_COPY. GL textures
 * become dma-bufs first (import_gl.c, ADR-2132). Nothing is ever copied through the host (the
 * planted VMAFX_TEST_FORCE_HOST_COPY defect does, and counts it).
 *
 * Ordering: a HIP_EVENT acquire fence is a wait enqueued on the library
 * stream, and the conversions and every reader follow it there. GL_SYNC,
 * SYNC_FILE and HOST acquire fences are checked on the host before anything
 * is enqueued: unsignalled, the import is VMAFX_E_BUSY and
 * vmafx_context_import_frame() waits and retries once (decision D8).
 */

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <hip/hip_runtime_api.h>

#include "common.h"
#include "hip_handle.h"
#include "log.h"
#include "picture.h"
#include "vmafx/error_internal.h"
#include "vmafx/frame_import_hooks.h"
#include "vmafx/internal.h"
#include "vmafx/vmafx.h"
#include "vmafx_hip.h"
#include "vmafx_hip_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Row alignment of the planes an import allocates. */
#define VMAFX_HIP_PITCH_ALIGN 256u
/* Block of the conversion kernels. */
#define VMAFX_HIP_BLOCK_X 32u
#define VMAFX_HIP_BLOCK_Y 8u

/* Where each plane of the frame comes from. */
typedef struct HipPlan {
    unsigned pw[3]; /* planar geometry of the frame */
    unsigned ph[3];
    uint32_t n_out;   /* planes of the frame: 1 (YUV400P) or 3 */
    size_t bytes;     /* per sample */
    bool owned[3];    /* plane i is in the frame's own allocation */
    bool staged[3];   /* ... the planted host copy */
    size_t offset[3]; /* its offset there */
    size_t pitch[3];  /* its pitch */
    size_t staging;   /* interleaved chroma read out of an array, SIZE_MAX: none */
    size_t staging_pitch;
    size_t total; /* bytes of the allocation */
} HipPlan;

/* What the import binds and from where. */
typedef struct HipSource {
    const VmafxFrameImport *d;
    const VmafxImportLayout *layout;
    void *base[3];        /* producer plane i's first sample (pointer and dma-buf imports) */
    hipArray_t arrays[3]; /* the planes of an array or GL import */
    bool from_arrays;
} HipSource;

const char *vmafx_hip_refusal(const char *extractor)
{
    /* Every HIP twin reads a HIP device picture: through
     * vmaf_hip_picture_upload() / vmaf_hip_picture_upload_staged() (the
     * shared frame, the private planes, cambi, SpEED), its own device copies
     * (psnr_hvs, ssimulacra2) or its level-0 kernel (float_ms_ssim), all
     * enqueued on the picture's library stream (ADR-2092). */
    (void)extractor;
    return NULL;
}

/* ---- Checks ------------------------------------------------------------------- */

static VmafxStatus check_hip_memory(const VmafxReport *report, const VmafxFrameImport *d,
                                    const VmafxImportLayout *layout)
{
    if ((layout->packed != VMAFX_IMPORT_PACKED_NONE || layout->msb) &&
        (d->memory == VMAFX_MEMORY_DEVICE_ARRAY || d->memory == VMAFX_MEMORY_GL_TEXTURE)) {
        return VMAFX_FAIL(
            report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
            "backend hip, memory %s, pixel format %s: a packed or MSB layout is linear "
            "words, not a HIP array or a GL texture of samples",
            vmafx_memory_kind_name(d->memory), layout->name);
    }
    switch (d->memory) {
    case VMAFX_MEMORY_DEVICE_POINTER:
    case VMAFX_MEMORY_DMABUF:
    case VMAFX_MEMORY_GL_TEXTURE: /* exported as dma-bufs: the copy rule is the export's */
        return VMAFX_OK;
    case VMAFX_MEMORY_DEVICE_ARRAY:
        if (layout->interleaved || (d->flags & VMAFX_IMPORT_ALLOW_COPY)) {
            return VMAFX_OK;
        }
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                          "backend hip, memory %s, pixel format %s: the twins read linear "
                          "device memory, so a planar frame in HIP arrays is a device copy, made "
                          "only with VMAFX_IMPORT_ALLOW_COPY (semi-planar arrays are "
                          "converted without it)",
                          vmafx_memory_kind_name(d->memory), layout->name);
    default:
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                          "backend hip, memory %s, pixel format %s: a HIP device binds "
                          "DEVICE_POINTER, DMABUF, DEVICE_ARRAY and GL_TEXTURE memory (host "
                          "frames go to vmafx_frame_wrap_host(); a host copy is never made)",
                          vmafx_memory_kind_name(d->memory), layout->name);
    }
}

/* A plane held in a HIP array or a GL texture: a handle, read whole. */
static VmafxStatus check_array_plane(const VmafxReport *report, const VmafxImportPlane *p,
                                     uint32_t i)
{
    if (p->handle == 0u) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, "handle"), "plane %u has no array or texture",
                          (unsigned)i);
    }
    if (p->modifier != 0u || p->plane_index != 0u) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PLANE,
                          vmafx_import_plane_field(i, p->modifier ? "modifier" : "plane_index"),
                          "plane %u: modifier 0x%llx, plane index %u; an array or texture is "
                          "read whole (modifier 0, plane index 0)",
                          (unsigned)i, (unsigned long long)p->modifier, (unsigned)p->plane_index);
    }
    return VMAFX_OK;
}

/* One producer plane of the import's memory kind. */
static VmafxStatus check_plane(const VmafxReport *report, const VmafxFrameImport *d, uint32_t i,
                               uint64_t row, uint64_t rows)
{
    switch (d->memory) {
    case VMAFX_MEMORY_DEVICE_POINTER:
        return vmafx_import_check_linear_plane(report, &d->plane[i], i, row, rows, "HIP device");
    case VMAFX_MEMORY_DMABUF:
        return vmafx_hip_dmabuf_check_plane(report, &d->plane[i], i, row, rows);
    default:
        return check_array_plane(report, &d->plane[i], i);
    }
}

static VmafxStatus check_hip_planes(const VmafxReport *report, const VmafxFrameImport *d,
                                    const VmafxImportLayout *layout)
{
    unsigned pw[3];
    unsigned ph[3];
    vmaf_picture_plane_extents(vmafx_engine_pixel_format(layout->planar_fmt), d->w, d->h, pw, ph);
    for (uint32_t i = 0; i < layout->n_planes; i++) {
        uint64_t row = 0;
        uint64_t rows = 0;
        vmafx_import_plane_extent(layout, d->bpc, i, pw, ph, &row, &rows);
        const VmafxStatus status = check_plane(report, d, i, row, rows);
        if (status != VMAFX_OK) {
            return status;
        }
    }
    return VMAFX_OK;
}

/* A HOST acquire fence: signalled (or planted as skipped), else BUSY. */
static VmafxStatus check_host_acquire(const VmafxReport *report, const VmafxFence *acquire)
{
    VmafxHostFence *host = NULL;
    const VmafxStatus status = vmafx_host_fence_of(report, acquire, "desc.acquire.handle", &host);
    if (status != VMAFX_OK || vmafx_host_fence_signalled(host) ||
        vmafx_test_switch(VMAFX_TEST_SKIP_ACQUIRE_WAIT)) {
        return status;
    }
    return VMAFX_FAIL(report, VMAFX_E_BUSY, 0, VMAFX_SUBJECT_FENCE, "desc.acquire",
                      "the producer has not signalled the HOST acquire fence; the HIP device "
                      "waits on HIP_EVENT fences on its stream (vmafx_context_import_frame() "
                      "waits and retries once)");
}

/* The acquire fence is one the HIP device honours: a HIP event (a wait on
 * the library stream), a GL sync of a GL import, a sync_file, a host fence
 * (the last three checked here, on the host), or none. */
static VmafxStatus check_hip_acquire(const VmafxReport *report, const VmafxFrameImport *d)
{
    const VmafxFence *const acquire = &d->acquire;
    switch (acquire->kind) {
    case VMAFX_FENCE_NONE:
        return VMAFX_OK;
    case VMAFX_FENCE_HIP_EVENT:
        return acquire->handle != 0u ?
                   VMAFX_OK :
                   VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE,
                              "desc.acquire.handle",
                              "backend hip: an acquire fence without its handle");
    case VMAFX_FENCE_GL_SYNC:
        if (d->memory != VMAFX_MEMORY_GL_TEXTURE) {
            return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE, "desc.acquire.kind",
                              "backend hip: a GL sync orders GL_TEXTURE imports only");
        }
        return VMAFX_OK; /* checked with the GL context current, in import_gl.c */
    case VMAFX_FENCE_SYNC_FILE:
        return vmafx_sync_file_acquire(report, acquire, "hip");
    case VMAFX_FENCE_HOST:
        return check_host_acquire(report, acquire);
    default:
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_FENCE, "desc.acquire.kind",
                          "backend hip: an acquire fence of kind %u; the HIP device waits on "
                          "NONE, HOST, HIP_EVENT, GL_SYNC and SYNC_FILE fences",
                          (unsigned)acquire->kind);
    }
}

/* ---- Planning ----------------------------------------------------------------- */

static size_t pitch_of(unsigned w, size_t bytes)
{
    const size_t row = (size_t)w * bytes;
    return (row + VMAFX_HIP_PITCH_ALIGN - 1u) & ~(size_t)(VMAFX_HIP_PITCH_ALIGN - 1u);
}

/* Planes of the frame the import converts (semi-planar chroma, P010 luma). */
static bool converted_plane(const VmafxImportLayout *layout, uint32_t i)
{
    return layout->shift != 0u || layout->msb || layout->packed != VMAFX_IMPORT_PACKED_NONE ||
           (i > 0u && layout->interleaved);
}

/* The producer plane frame plane `i` is read from. */
static uint32_t source_plane(const VmafxImportLayout *layout, uint32_t i)
{
    if (layout->packed != VMAFX_IMPORT_PACKED_NONE) {
        return 0u;
    }
    return layout->interleaved && i > 0u ? 1u : i;
}

/* Which planes the frame owns: converted (semi-planar chroma, the P010
 * luma), read out of arrays, or (planted) staged through the host; the rest
 * bound. */
static void plan_planes(const HipSource *src, bool force_copy, HipPlan *plan)
{
    const VmafxFrameImport *const d = src->d;
    const VmafxImportLayout *const layout = src->layout;
    vmaf_picture_plane_extents(vmafx_engine_pixel_format(layout->planar_fmt), d->w, d->h, plan->pw,
                               plan->ph);
    plan->n_out = layout->planar_fmt == VMAFX_PIXEL_FORMAT_YUV400P ? 1u : 3u;
    plan->bytes = d->bpc > 8u ? 2u : 1u;
    plan->total = 0;
    for (uint32_t i = 0; i < 3u; i++) {
        const bool converted = converted_plane(layout, i);
        const bool linear = i < plan->n_out && !converted && !src->from_arrays;
        plan->staged[i] = linear && force_copy;
        plan->owned[i] = i < plan->n_out && (converted || plan->staged[i] || src->from_arrays);
        plan->pitch[i] = plan->owned[i] ? pitch_of(plan->pw[i], plan->bytes) : 0u;
        plan->offset[i] = plan->total;
        plan->total += plan->pitch[i] * plan->ph[i];
    }
    plan->staging = SIZE_MAX;
    plan->staging_pitch = 0;
    if (src->from_arrays && layout->interleaved) {
        plan->staging = plan->total;
        plan->staging_pitch = pitch_of(2u * plan->pw[1], plan->bytes);
        plan->total += plan->staging_pitch * plan->ph[1];
    }
}

/* ---- Device work ---------------------------------------------------------------- */

static hipError_t launch_2d(const VmafxHipDevice *dev, hipFunction_t fn, unsigned w, unsigned h,
                            void **args)
{
    if (!fn) {
        return hipErrorNotSupported; /* a build without device code */
    }
    return hipModuleLaunchKernel(fn, (w + VMAFX_HIP_BLOCK_X - 1u) / VMAFX_HIP_BLOCK_X,
                                 (h + VMAFX_HIP_BLOCK_Y - 1u) / VMAFX_HIP_BLOCK_Y, 1,
                                 VMAFX_HIP_BLOCK_X, VMAFX_HIP_BLOCK_Y, 1, 0, dev->str, args, NULL);
}

/* Cb and Cr of an interleaved plane (`src`, `pitch`) into frame planes 1, 2. */
static hipError_t deinterleave(const VmafxHipDevice *dev, const HipPlan *plan, uint8_t *base,
                               const void *src, size_t pitch, unsigned shift)
{
    void *cb = base + plan->offset[1];
    void *cr = base + plan->offset[2];
    size_t dst_pitch = plan->pitch[1];
    unsigned w = plan->pw[1];
    unsigned h = plan->ph[1];
    void *args8[] = {(void *)&src, &pitch, (void *)&cb, (void *)&cr, &dst_pitch, &w, &h};
    void *args16[] = {(void *)&src, &pitch, (void *)&cb, (void *)&cr, &dst_pitch, &w, &h, &shift};
    assert(plan->pitch[1] == plan->pitch[2]);
    return plan->bytes == 1u ? launch_2d(dev, dev->kernels.deint_8, w, h, args8) :
                               launch_2d(dev, dev->kernels.deint_16, w, h, args16);
}

/* The P010 luma, shifted into frame plane 0. */
static hipError_t shift_luma(const VmafxHipDevice *dev, const HipPlan *plan, uint8_t *base,
                             const void *src, size_t pitch, unsigned shift)
{
    void *dst = base + plan->offset[0];
    size_t dst_pitch = plan->pitch[0];
    unsigned w = plan->pw[0];
    unsigned h = plan->ph[0];
    void *args[] = {(void *)&src, &pitch, (void *)&dst, &dst_pitch, &w, &h, &shift};
    return launch_2d(dev, dev->kernels.shift_16, w, h, args);
}

/* Plane `i` of a packed layout or of MSB planar words, gathered from the
 * producer's plane by the plan the host reference uses
 * (vmafx_import_plane_read(), vmafx_import_read_plane()). */
static hipError_t gather_plane(const VmafxHipDevice *dev, const HipSource *src, const HipPlan *plan,
                               uint8_t *base, uint32_t i)
{
    VmafxImportRead rd;
    vmafx_import_plane_read(src->layout, src->d->bpc, i, &rd);
    const void *in = src->base[rd.src_plane];
    size_t in_pitch = (size_t)src->d->plane[rd.src_plane].pitch;
    void *out = base + plan->offset[i];
    size_t out_pitch = plan->pitch[i];
    unsigned w = plan->pw[i];
    unsigned h = plan->ph[i];
    unsigned out_bytes = plan->bytes;
    void *args[] = {(void *)&in, &in_pitch,  (void *)&out, &out_pitch, &w,       &h,
                    &rd.step,    &rd.offset, &rd.in_bytes, &rd.shift,  &rd.mask, &out_bytes};
    return launch_2d(dev, dev->kernels.gather, w, h, args);
}

/* The planted host-copy defect (VMAFX_TEST_FORCE_HOST_COPY): a device plane
 * staged through host memory into a plane of the frame's own. Counted, so
 * the tests that assert no host copy fail when it is planted. */
static hipError_t stage_through_host(const VmafxHipDevice *dev, const void *src, size_t src_pitch,
                                     void *dst, size_t dst_pitch, size_t row, size_t rows)
{
    uint8_t *const host = malloc(row * rows);
    if (!host) {
        return hipErrorOutOfMemory; /* no host memory for the planted copy */
    }
    hipError_t rc =
        hipMemcpy2DAsync(host, row, src, src_pitch, row, rows, hipMemcpyDeviceToHost, dev->str);
    rc = rc == hipSuccess ? hipStreamSynchronize(dev->str) : rc;
    rc = rc == hipSuccess ? hipMemcpy2DAsync(dst, dst_pitch, host, row, row, rows,
                                             hipMemcpyHostToDevice, dev->str) :
                            rc;
    rc = rc == hipSuccess ? hipStreamSynchronize(dev->str) : rc;
    free(host);
    vmafx_count_host_copy((uint64_t)(row * rows));
    return rc;
}

/* Frame plane `i` from linear producer memory: bound, converted or (planted)
 * staged. */
static hipError_t fill_linear_plane(const VmafxHipDevice *dev, const HipSource *src,
                                    const HipPlan *plan, uint8_t *base, uint32_t i, void *data[3])
{
    const VmafxImportLayout *const layout = src->layout;
    const bool pair = layout->interleaved && i > 0u;
    const uint32_t from = source_plane(layout, i);
    const size_t pitch = (size_t)src->d->plane[from].pitch;
    if (!plan->owned[i]) {
        data[i] = src->base[from]; /* bound */
        return hipSuccess;
    }
    if (base == NULL) {
        return hipErrorInvalidValue; /* an owned plane needs the frame's memory */
    }
    data[i] = (void *)(base + plan->offset[i]);
    if (layout->packed != VMAFX_IMPORT_PACKED_NONE || layout->msb) {
        return gather_plane(dev, src, plan, base, i);
    }
    if (i == 2u && pair) {
        return hipSuccess; /* written with plane 1 */
    }
    if (pair) {
        return deinterleave(dev, plan, base, src->base[1], pitch, layout->shift);
    }
    if (layout->shift != 0u) {
        return shift_luma(dev, plan, base, src->base[0], pitch, layout->shift);
    }
    assert(plan->staged[i]);
    return stage_through_host(dev, src->base[i], pitch, data[i], plan->pitch[i],
                              (size_t)plan->pw[i] * plan->bytes, plan->ph[i]);
}

/* Frame plane `i` out of arrays: the luma and planar planes read out, the
 * P010 luma shifted in place, the interleaved chroma read into the staging
 * rows and de-interleaved. */
static hipError_t fill_array_plane(const VmafxHipDevice *dev, const HipSource *src,
                                   const HipPlan *plan, uint8_t *base, uint32_t i, void *data[3])
{
    const VmafxImportLayout *const layout = src->layout;
    data[i] = base + plan->offset[i];
    if (layout->interleaved && i == 2u) {
        return hipSuccess; /* written with plane 1 */
    }
    if (layout->interleaved && i == 1u) {
        uint8_t *const staging = base + plan->staging;
        const hipError_t rc = hipMemcpy2DFromArrayAsync(
            staging, plan->staging_pitch, src->arrays[1], 0, 0,
            2u * (size_t)plan->pw[1] * plan->bytes, plan->ph[1], hipMemcpyDeviceToDevice, dev->str);
        return rc == hipSuccess ?
                   deinterleave(dev, plan, base, staging, plan->staging_pitch, layout->shift) :
                   rc;
    }
    const hipError_t rc = hipMemcpy2DFromArrayAsync(data[i], plan->pitch[i], src->arrays[i], 0, 0,
                                                    (size_t)plan->pw[i] * plan->bytes, plan->ph[i],
                                                    hipMemcpyDeviceToDevice, dev->str);
    if (rc != hipSuccess || layout->shift == 0u) {
        return rc;
    }
    return shift_luma(dev, plan, base, data[i], plan->pitch[i], layout->shift);
}

/* The frame's own planes, stream-ordered on the library stream. */
static VmafxStatus alloc_owned(const VmafxReport *report, VmafxHipFrame *hf, const HipPlan *plan)
{
    if (plan->total == 0u) {
        return VMAFX_OK;
    }
    const hipError_t rc = hipMallocAsync(&hf->owned, plan->total, hf->dev->str);
    if (rc == hipSuccess) {
        return VMAFX_OK;
    }
    hf->owned = NULL;
    return VMAFX_FAIL(report, vmafx_hip_status(rc), (int32_t)rc, VMAFX_SUBJECT_FRAME, "frame",
                      "backend hip: cannot allocate %zu bytes of converted planes: %s (%d)",
                      plan->total, hipGetErrorName(rc), (int)rc);
}

/* Log once per device that VMAFX_IMPORT_ALLOW_COPY made a device copy. */
static void note_device_copy(VmafxHipDevice *dev, const VmafxFrameImport *d)
{
    if (atomic_exchange(&dev->copy_logged, 1u) == 0u) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "vmafx: backend hip device %d: VMAFX_IMPORT_ALLOW_COPY: the planes of %s "
                 "imports are copied on the device (no host copy)\n",
                 (int)dev->index, vmafx_memory_kind_name(d->memory));
    }
}

/* A conversion of a build without device code: refused, named. */
static VmafxStatus no_kernels(const VmafxReport *report, const HipSource *src)
{
    return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.pix_fmt",
                      "backend hip, pixel format %s: this build has no HIP device code "
                      "(enable_hipcc=false) to convert it on the device",
                      src->layout->name);
}

/* A read-out of the producer's plane that failed, named. */
static VmafxStatus fill_failed(const VmafxReport *report, const HipSource *src, hipError_t rc)
{
    if (rc == hipErrorNotSupported) {
        return no_kernels(report, src);
    }
    return VMAFX_FAIL(report, VMAFX_E_DEVICE, (int32_t)rc, VMAFX_SUBJECT_FRAME, "frame",
                      "backend hip: cannot enqueue the planes of the import: %s (%d)",
                      hipGetErrorName(rc), (int)rc);
}

/* Every plane of the frame into `data` / `stride`, the work on the library
 * stream behind the acquire wait. */
static VmafxStatus fill_planes(const VmafxReport *report, VmafxHipFrame *hf, const HipSource *src,
                               void *data[3], ptrdiff_t stride[3])
{
    HipPlan plan;
    const bool force_copy = !src->from_arrays && vmafx_test_switch(VMAFX_TEST_FORCE_HOST_COPY);
    plan_planes(src, force_copy, &plan);
    VmafxStatus status = alloc_owned(report, hf, &plan);
    for (uint32_t i = 0; i < plan.n_out && status == VMAFX_OK; i++) {
        const hipError_t rc = src->from_arrays ?
                                  fill_array_plane(hf->dev, src, &plan, hf->owned, i, data) :
                                  fill_linear_plane(hf->dev, src, &plan, hf->owned, i, data);
        stride[i] = (ptrdiff_t)(plan.owned[i] ? plan.pitch[i] : src->d->plane[i].pitch);
        if (rc != hipSuccess) {
            status = fill_failed(report, src, rc);
        }
    }
    if (status == VMAFX_OK &&
        (converted_plane(src->layout, 0u) || converted_plane(src->layout, 1u))) {
        vmafx_count_conversion();
    }
    if (status == VMAFX_OK && src->from_arrays && !src->layout->interleaved) {
        note_device_copy(hf->dev, src->d);
    }
    return status;
}

/* ---- Binding ------------------------------------------------------------------ */

int vmafx_hip_frame_release(VmafxFrame *frame, VmafPicture *pic)
{
    VmafxHipFrame *const hf = frame->lane;
    assert(hf != NULL && hf->dev != NULL && pic != NULL);
    (void)pic;
    VmafxHostFence *const fence = atomic_exchange(&frame->released, (VmafxHostFence *)NULL);
    frame->lane = NULL;
    int err = vmafx_hip_bind(hf->dev);
    if (err) {
        /* The release still runs: the stream calls name their device. */
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vmafx: backend hip: cannot bind the device (%d)\n", err);
    }
    const int released = vmafx_hip_release_frame(hf, fence);
    return err ? err : released;
}

/* The acquire wait on the library stream (none under the planted defect). */
static VmafxStatus wait_acquire(const VmafxReport *report, const VmafxHipDevice *dev,
                                const VmafxFence *acquire)
{
    if (acquire->kind != VMAFX_FENCE_HIP_EVENT || vmafx_test_switch(VMAFX_TEST_SKIP_ACQUIRE_WAIT)) {
        return VMAFX_OK;
    }
    const hipError_t rc = hipStreamWaitEvent(dev->str, vmaf_hip_event_of(acquire->handle), 0u);
    if (rc == hipSuccess) {
        return VMAFX_OK;
    }
    return VMAFX_FAIL(report, VMAFX_E_INVALID, (int32_t)rc, VMAFX_SUBJECT_FENCE,
                      "desc.acquire.handle",
                      "backend hip: cannot wait on the acquire event 0x%llx: %s (%d); is it an "
                      "event of the device?",
                      (unsigned long long)acquire->handle, hipGetErrorName(rc), (int)rc);
}

/* The producer's memory where the twins and conversions read it. */
static VmafxStatus locate_planes(const VmafxReport *report, VmafxHipFrame *hf, HipSource *src)
{
    const VmafxFrameImport *const d = src->d;
    switch (d->memory) {
    case VMAFX_MEMORY_DEVICE_ARRAY:
        for (uint32_t i = 0; i < src->layout->n_planes; i++) {
            /* NOLINTNEXTLINE(performance-no-int-to-ptr): the producer's hipArray_t crosses the ABI as uintptr_t (VmafxImportPlane.handle, ADR-1929). */
            src->arrays[i] = (hipArray_t)d->plane[i].handle;
        }
        return VMAFX_OK;
    case VMAFX_MEMORY_DMABUF:
        return vmafx_hip_dmabuf_map(report, hf, d, src->layout->n_planes, src->base);
    default:
        for (uint32_t i = 0; i < src->layout->n_planes; i++) {
            /* NOLINTNEXTLINE(performance-no-int-to-ptr): the producer's device pointer crosses the ABI as uintptr_t (VmafxImportPlane.handle, ADR-1929). */
            src->base[i] = (uint8_t *)d->plane[i].handle + d->plane[i].offset;
        }
        return VMAFX_OK;
    }
}

/* The planes behind the acquire fence, the device bound. */
static VmafxStatus enqueue_import(const VmafxReport *report, VmafxHipFrame *hf, HipSource *src,
                                  void *data[3], ptrdiff_t stride[3])
{
    VmafxStatus status = locate_planes(report, hf, src);
    if (status == VMAFX_OK) {
        status = wait_acquire(report, hf->dev, &src->d->acquire);
    }
    return status == VMAFX_OK ? fill_planes(report, hf, src, data, stride) : status;
}

/* The VmafxFrame over the planes, a HIP device picture read on the library
 * stream. */
static VmafxStatus bind_picture(const VmafxReport *report, const VmafxHipFrame *hf,
                                const HipSource *src, void *data[3], ptrdiff_t stride[3],
                                VmafxFrame *frame)
{
    VmafxFrameDesc planar = VMAFX_FRAME_DESC_INIT;
    planar.pix_fmt = src->layout->planar_fmt;
    planar.bpc = src->d->bpc;
    planar.w = src->d->w;
    planar.h = src->d->h;
    const int err = vmafx_frame_bind(frame, &planar, data, stride);
    if (err) {
        return VMAFX_FAIL(report, err == -ENOMEM ? VMAFX_E_NOMEM : VMAFX_E_DEVICE, err,
                          VMAFX_SUBJECT_FRAME, "frame", "backend hip: cannot bind the frame (%d)",
                          err);
    }
    VmafPicturePrivate *const priv = frame->pic.priv;
    priv->buf_type = VMAF_PICTURE_BUFFER_TYPE_HIP_DEVICE;
    priv->hip.str = vmaf_hip_stream_bits(hf->dev->str);
    return VMAFX_OK;
}

/* Undo a failed import: whatever was enqueued finishes before the
 * stream-ordered free. */
static void unwind_import(VmafxHipFrame *hf)
{
    (void)vmafx_hip_release_frame(hf, NULL);
}

/* Bind a checked descriptor: lane state, planes, picture. */
static VmafxStatus bind_hip_frame(const VmafxReport *report, VmafxDevice *device, HipSource *src,
                                  VmafxFrame **out)
{
    VmafxHipDevice *const dev = vmafx_hip_dev(device);
    vmafx_hip_graves_reap(dev, false);
    VmafxHipFrame *const hf = vmafx_hip_frame_state_new(dev);
    VmafxFrame *const frame = hf ? calloc(1, sizeof(*frame)) : NULL;
    if (!frame || vmafx_hip_bind(dev) != 0) {
        free(frame);
        vmafx_hip_frame_state_free(hf);
        return VMAFX_FAIL(report, frame ? VMAFX_E_DEVICE : VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_FRAME,
                          "frame", "backend hip: cannot start the import");
    }
    void *data[3] = {NULL, NULL, NULL};
    ptrdiff_t stride[3] = {0, 0, 0};
    VmafxStatus status = enqueue_import(report, hf, src, data, stride);
    /* Submit the import's work now. On gfx1036 (ROCm 7.2.4, measured)
     * device copies enqueued on the library stream later, by the twins,
     * overtook an array readout of the import that was still unsubmitted:
     * the last frame of a session read stale planes (3 to 5 values wrong in
     * 5 of 6 runs; 0 of 8 with this query, which submits the stream's
     * batched commands). T-HIP-GFX1036-UNFLUSHED-STREAM-ORDER-2026-10-06. */
    (void)hipStreamQuery(hf->dev->str);
    if (status == VMAFX_OK) {
        status = bind_picture(report, hf, src, data, stride, frame);
    }
    if (status != VMAFX_OK) {
        unwind_import(hf);
        free(frame);
        return status;
    }
    assert(frame->pic.priv != NULL && frame->pic.ref != NULL);
    frame->lane = hf;
    frame->lane_release = vmafx_hip_frame_release;
    frame->release = src->d->release;
    frame->user = src->d->user;
    frame->residency = VMAFX_BACKEND_HIP;
    frame->device = vmafx_device_ref(device);
    *out = frame;
    return VMAFX_OK;
}

/* A GL_TEXTURE import: the textures' dma-bufs (a GPU copy into linear ones
 * where the driver exports a tiled layout), imported as a DMABUF frame
 * (ADR-2132). The GL sync is checked first, before anything is exported. */
static VmafxStatus import_gl(const VmafxReport *report, VmafxDevice *device,
                             const VmafxFrameImport *desc, const VmafxImportLayout *layout,
                             VmafxFrame **out)
{
    VmafxStatus status = vmafx_gl_sync_acquire(report, &desc->acquire, "hip");
    if (status != VMAFX_OK) {
        return status;
    }
    VmafxFrameImport dmabuf;
    bool copied = false;
    status = vmafx_hip_gl_export(report, vmafx_hip_dev(device), desc, layout, &dmabuf, &copied);
    if (status != VMAFX_OK) {
        return status;
    }
    status = check_hip_planes(report, &dmabuf, layout);
    if (status == VMAFX_OK) {
        if (copied) {
            note_device_copy(vmafx_hip_dev(device), desc);
        }
        HipSource src;
        memset(&src, 0, sizeof(src));
        src.d = &dmabuf;
        src.layout = layout;
        status = bind_hip_frame(report, device, &src, out);
    }
    vmafx_hip_gl_close(&dmabuf, layout->n_planes); /* the import duplicated them */
    return status;
}

VmafxStatus vmafx_hip_frame_import(const VmafxReport *report, VmafxDevice *device,
                                   const VmafxFrameImport *desc, const VmafxImportLayout *layout,
                                   VmafxFrame **out)
{
    VmafxStatus status = check_hip_memory(report, desc, layout);
    if (status == VMAFX_OK) {
        status = check_hip_planes(report, desc, layout);
    }
    if (status == VMAFX_OK) {
        status = check_hip_acquire(report, desc);
    }
    if (status != VMAFX_OK) {
        return status;
    }
    if (desc->memory == VMAFX_MEMORY_GL_TEXTURE) {
        return import_gl(report, device, desc, layout, out);
    }
    HipSource src;
    memset(&src, 0, sizeof(src));
    src.d = desc;
    src.layout = layout;
    src.from_arrays = desc->memory == VMAFX_MEMORY_DEVICE_ARRAY;
    return bind_hip_frame(report, device, &src, out);
}

/* NOLINTEND(modernize-use-nullptr) */
