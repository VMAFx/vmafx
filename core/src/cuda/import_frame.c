/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Imported frames on a CUDA device (RC4 WP3, ADR-1852 design section 2.7,
 * ADR-1929, ADR-2023).
 *
 * Device pointers are bound where they are: the planes of a planar frame are
 * the producer's memory, with no device-to-device copy into a pool. NV12 /
 * P010 / P016 are planarised on the device into planes of the frame's own
 * (import_convert.cu: a de-interleave and the P010 shift, nothing else), so
 * an imported frame scores bit for bit as the same frame uploaded from the
 * host. CUDA arrays and GL textures (import_gl.c) are not linear memory the
 * extractors read: a semi-planar frame in arrays is converted the same way,
 * a planar one is copied on the device only with VMAFX_IMPORT_ALLOW_COPY.
 * Nothing is ever copied through the host (the planted
 * VMAFX_TEST_FORCE_HOST_COPY defect does, and counts it).
 *
 * Every frame is read on the device's library stream (ADR-2023): the acquire
 * fence is a wait enqueued there, the conversions follow it, and the frame's
 * ready event is recorded behind them, so no reader starts before the
 * producer's write and the ADR-1199 context barrier is not needed.
 */

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "cuda_helper.cuh"
#include "log.h"
#include "picture.h"
#include "vmafx/error_internal.h"
#include "vmafx/frame_import_hooks.h"
#include "vmafx/internal.h"
#include "vmafx/vmafx.h"
#include "vmafx_cuda.h"
#include "vmafx_cuda_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Row alignment of the planes an import allocates (cuMemAllocPitch's). */
#define VMAFX_CUDA_PITCH_ALIGN 512u
/* Block of the conversion kernels. */
#define VMAFX_CUDA_BLOCK_X 32u
#define VMAFX_CUDA_BLOCK_Y 8u

/* Where each plane of the frame comes from. */
typedef struct CudaPlan {
    unsigned pw[3]; /* planar geometry of the frame */
    unsigned ph[3];
    uint32_t n_out;   /* planes of the frame: 1 (YUV400P) or 3 */
    size_t bytes;     /* per sample */
    bool owned[3];    /* plane i is in the frame's own allocation */
    bool copied[3];   /* ... a device copy of an unaligned producer plane */
    bool staged[3];   /* ... the planted host copy */
    size_t offset[3]; /* its offset there */
    size_t pitch[3];  /* its pitch */
    size_t staging;   /* interleaved chroma read out of an array, SIZE_MAX: none */
    size_t staging_pitch;
    size_t total; /* bytes of the allocation */
} CudaPlan;

/* What the import binds and from where. */
typedef struct CudaSource {
    const VmafxFrameImport *d;
    const VmafxImportLayout *layout;
    CUarray arrays[3]; /* the planes of an array or GL import, else 0 */
    bool from_arrays;
} CudaSource;

const char *vmafx_cuda_refusal(const char *extractor)
{
    /* Every CUDA twin reads a CUDA device picture on the picture's stream
     * (the audit of ADR-2023: integer_adm and integer_cambi read the
     * distorted picture on its own stream, which for an imported frame is
     * the same library stream). */
    (void)extractor;
    return NULL;
}

/* ---- Checks ------------------------------------------------------------------- */

static VmafxStatus check_cuda_memory(const VmafxReport *report, const VmafxFrameImport *d,
                                     const VmafxImportLayout *layout)
{
    switch (d->memory) {
    case VMAFX_MEMORY_DEVICE_POINTER:
        return VMAFX_OK;
    case VMAFX_MEMORY_DEVICE_ARRAY:
    case VMAFX_MEMORY_GL_TEXTURE:
        if (layout->interleaved || (d->flags & VMAFX_IMPORT_ALLOW_COPY)) {
            return VMAFX_OK;
        }
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                          "backend cuda, memory %s, pixel format %s: the extractors read linear "
                          "device memory, so a planar frame in CUDA arrays is a device copy, "
                          "made only with VMAFX_IMPORT_ALLOW_COPY (NV12 / P010 / P016 arrays are "
                          "converted without it)",
                          vmafx_memory_kind_name(d->memory), layout->name);
    default:
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                          "backend cuda, memory %s, pixel format %s: a CUDA device binds "
                          "DEVICE_POINTER, DEVICE_ARRAY and GL_TEXTURE memory (host frames go "
                          "to vmafx_frame_wrap_host(); a host copy is never made)",
                          vmafx_memory_kind_name(d->memory), layout->name);
    }
}

/* A bound plane is read as the CUDA twins read planes they allocate: rows
 * start 8-byte aligned and are loaded 8 bytes at a time (integer_vif's
 * vertical pass loads uint2), so a row's last load may reach up to 7 bytes
 * past the row inside the pitch. */
#define VMAFX_CUDA_ROW_ALIGN 8u

static uint64_t row_rounded(uint64_t row)
{
    return (row + VMAFX_CUDA_ROW_ALIGN - 1u) & ~(uint64_t)(VMAFX_CUDA_ROW_ALIGN - 1u);
}

/* The plane may be bound where it is: aligned rows with room for the last
 * load of each, inside the memory object when the producer gave its size
 * (vmafx_import_check_linear_plane() proved offset <= size). */
static bool bindable(const VmafxImportPlane *p, uint64_t row, uint64_t rows)
{
    const bool aligned = ((p->handle + p->offset) % VMAFX_CUDA_ROW_ALIGN) == 0u &&
                         (p->pitch % VMAFX_CUDA_ROW_ALIGN) == 0u && p->pitch >= row_rounded(row);
    return aligned &&
           (p->size == 0u || (rows - 1u) * p->pitch + row_rounded(row) <= p->size - p->offset);
}

/* Planes of the frame the import converts (semi-planar chroma, P010 luma):
 * read a byte at a time, so any address and pitch. */
static bool converted_plane(const VmafxImportLayout *layout, uint32_t i)
{
    return layout->shift != 0u || (i > 0u && layout->interleaved);
}

/* A plane of device pointer memory: linear, inside the address space, and
 * either converted, bindable, or copied on the device with
 * VMAFX_IMPORT_ALLOW_COPY. */
static VmafxStatus check_pointer_plane(const VmafxReport *report, const VmafxFrameImport *d,
                                       const VmafxImportLayout *layout, uint32_t i, uint64_t row,
                                       uint64_t rows)
{
    const VmafxImportPlane *const p = &d->plane[i];
    const VmafxStatus status =
        vmafx_import_check_linear_plane(report, p, i, row, rows, "CUDA device");
    if (status != VMAFX_OK || converted_plane(layout, i) || bindable(p, row, rows) ||
        (d->flags & VMAFX_IMPORT_ALLOW_COPY)) {
        return status;
    }
    const bool aligned = ((p->handle + p->offset) % VMAFX_CUDA_ROW_ALIGN) == 0u;
    return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PLANE,
                      vmafx_import_plane_field(i, aligned ? "pitch" : "offset"),
                      "plane %u: address 0x%llx + %llu, pitch %llu; the CUDA twins read rows "
                      "%u-byte aligned, %u bytes at a time, so a bound plane needs an aligned "
                      "address and pitch and a pitch of at least %llu bytes "
                      "(VMAFX_IMPORT_ALLOW_COPY copies it on the device)",
                      (unsigned)i, (unsigned long long)p->handle, (unsigned long long)p->offset,
                      (unsigned long long)p->pitch, VMAFX_CUDA_ROW_ALIGN, VMAFX_CUDA_ROW_ALIGN,
                      (unsigned long long)row_rounded(row));
}

/* A plane held in a CUDA array or a GL texture: a handle, linear (the array
 * itself is the layout), its first level. */
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

static VmafxStatus check_cuda_planes(const VmafxReport *report, const VmafxFrameImport *d,
                                     const VmafxImportLayout *layout)
{
    unsigned pw[3];
    unsigned ph[3];
    vmaf_picture_plane_extents(vmafx_engine_pixel_format(layout->planar_fmt), d->w, d->h, pw, ph);
    for (uint32_t i = 0; i < layout->n_planes; i++) {
        uint64_t row = 0;
        uint64_t rows = 0;
        vmafx_import_plane_extent(layout, d->bpc, i, pw, ph, &row, &rows);
        const VmafxStatus status = d->memory == VMAFX_MEMORY_DEVICE_POINTER ?
                                       check_pointer_plane(report, d, layout, i, row, rows) :
                                       check_array_plane(report, &d->plane[i], i);
        if (status != VMAFX_OK) {
            return status;
        }
    }
    return VMAFX_OK;
}

/* The acquire fence is one the CUDA device honours: a CUDA event (a wait on
 * the library stream), a GL sync of a GL import, a host fence already
 * signalled, or none. */
static VmafxStatus check_cuda_acquire(const VmafxReport *report, const VmafxFrameImport *d)
{
    const VmafxFence *const acquire = &d->acquire;
    switch (acquire->kind) {
    case VMAFX_FENCE_NONE:
        return VMAFX_OK;
    case VMAFX_FENCE_CUDA_EVENT:
    case VMAFX_FENCE_GL_SYNC:
        if (acquire->handle == 0u ||
            (acquire->kind == VMAFX_FENCE_GL_SYNC && d->memory != VMAFX_MEMORY_GL_TEXTURE)) {
            return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE,
                              acquire->handle ? "desc.acquire.kind" : "desc.acquire.handle",
                              "backend cuda: %s",
                              acquire->handle ? "a GL sync orders GL_TEXTURE "
                                                "imports only" :
                                                "an acquire fence without its "
                                                "handle");
        }
        return VMAFX_OK;
    case VMAFX_FENCE_HOST: {
        VmafxHostFence *host = NULL;
        const VmafxStatus status =
            vmafx_host_fence_of(report, acquire, "desc.acquire.handle", &host);
        if (status != VMAFX_OK || vmafx_host_fence_signalled(host) ||
            vmafx_test_switch(VMAFX_TEST_SKIP_ACQUIRE_WAIT)) {
            return status;
        }
        return VMAFX_FAIL(report, VMAFX_E_BUSY, 0, VMAFX_SUBJECT_FENCE, "desc.acquire",
                          "the producer has not signalled the HOST acquire fence; the CUDA "
                          "device waits on CUDA_EVENT fences on its stream "
                          "(vmafx_context_import_frame() waits and retries once)");
    }
    default:
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_FENCE, "desc.acquire.kind",
                          "backend cuda: an acquire fence of kind %u; the CUDA device waits on "
                          "NONE, HOST, CUDA_EVENT and GL_SYNC fences",
                          (unsigned)acquire->kind);
    }
}

/* ---- Planning ----------------------------------------------------------------- */

static size_t pitch_of(unsigned w, size_t bytes)
{
    const size_t row = (size_t)w * bytes;
    return (row + VMAFX_CUDA_PITCH_ALIGN - 1u) & ~(size_t)(VMAFX_CUDA_PITCH_ALIGN - 1u);
}

/* Which planes the frame owns: converted (semi-planar chroma, the P010
 * luma), copied (arrays, or every plane under the planted host copy), the
 * rest bound. */
static void plan_planes(const CudaSource *src, bool force_copy, CudaPlan *plan)
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
        const bool pointer = i < plan->n_out && !converted && !src->from_arrays;
        uint64_t row = 0;
        uint64_t rows = 0;
        vmafx_import_plane_extent(layout, d->bpc, i < layout->n_planes ? i : 0u, plan->pw, plan->ph,
                                  &row, &rows);
        plan->staged[i] = pointer && force_copy;
        plan->copied[i] = pointer && !force_copy && !bindable(&d->plane[i], row, rows);
        plan->owned[i] = i < plan->n_out &&
                         (converted || plan->staged[i] || plan->copied[i] || src->from_arrays);
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

static CUresult launch_2d(const VmafxCudaDevice *dev, CUfunction fn, unsigned w, unsigned h,
                          void **args)
{
    return dev->state.f->cuLaunchKernel(fn, DIV_ROUND_UP(w, VMAFX_CUDA_BLOCK_X),
                                        DIV_ROUND_UP(h, VMAFX_CUDA_BLOCK_Y), 1, VMAFX_CUDA_BLOCK_X,
                                        VMAFX_CUDA_BLOCK_Y, 1, 0, dev->state.str, args, NULL);
}

/* Cb and Cr of an interleaved plane (`src`, `pitch`) into frame planes 1, 2. */
static CUresult deinterleave(const VmafxCudaDevice *dev, const CudaPlan *plan, CUdeviceptr base,
                             CUdeviceptr src, size_t pitch, unsigned shift)
{
    CUdeviceptr cb = base + plan->offset[1];
    CUdeviceptr cr = base + plan->offset[2];
    size_t dst_pitch = plan->pitch[1];
    unsigned w = plan->pw[1];
    unsigned h = plan->ph[1];
    void *args8[] = {&src, &pitch, &cb, &cr, &dst_pitch, &w, &h};
    void *args16[] = {&src, &pitch, &cb, &cr, &dst_pitch, &w, &h, &shift};
    assert(plan->pitch[1] == plan->pitch[2]);
    return plan->bytes == 1u ? launch_2d(dev, dev->kernels.deint_8, w, h, args8) :
                               launch_2d(dev, dev->kernels.deint_16, w, h, args16);
}

/* The P010 luma, shifted into frame plane 0. */
static CUresult shift_luma(const VmafxCudaDevice *dev, const CudaPlan *plan, CUdeviceptr base,
                           CUdeviceptr src, size_t pitch, unsigned shift)
{
    CUdeviceptr dst = base + plan->offset[0];
    size_t dst_pitch = plan->pitch[0];
    unsigned w = plan->pw[0];
    unsigned h = plan->ph[0];
    void *args[] = {&src, &pitch, &dst, &dst_pitch, &w, &h, &shift};
    return launch_2d(dev, dev->kernels.shift_16, w, h, args);
}

/* The planted host-copy defect (VMAFX_TEST_FORCE_HOST_COPY): a device plane
 * staged through host memory into a plane of the frame's own. Counted, so
 * the tests that assert no host copy fail when it is planted. */
static CUresult stage_through_host(const VmafxCudaDevice *dev, CUdeviceptr src, size_t src_pitch,
                                   CUdeviceptr dst, size_t dst_pitch, size_t row, size_t rows)
{
    CudaFunctions *const f = dev->state.f;
    uint8_t *const host = malloc(row * rows);
    if (!host) {
        return CUDA_ERROR_UNKNOWN; /* no host memory for the planted copy */
    }
    CUDA_MEMCPY2D down = {.srcMemoryType = CU_MEMORYTYPE_DEVICE,
                          .srcDevice = src,
                          .srcPitch = src_pitch,
                          .dstMemoryType = CU_MEMORYTYPE_HOST,
                          .dstHost = host,
                          .dstPitch = row,
                          .WidthInBytes = row,
                          .Height = rows};
    CUDA_MEMCPY2D up = {.srcMemoryType = CU_MEMORYTYPE_HOST,
                        .srcHost = host,
                        .srcPitch = row,
                        .dstMemoryType = CU_MEMORYTYPE_DEVICE,
                        .dstDevice = dst,
                        .dstPitch = dst_pitch,
                        .WidthInBytes = row,
                        .Height = rows};
    CUresult res = f->cuMemcpy2DAsync(&down, dev->state.str);
    res = res == CUDA_SUCCESS ? f->cuStreamSynchronize(dev->state.str) : res;
    res = res == CUDA_SUCCESS ? f->cuMemcpy2DAsync(&up, dev->state.str) : res;
    res = res == CUDA_SUCCESS ? f->cuStreamSynchronize(dev->state.str) : res;
    free(host);
    vmafx_count_host_copy((uint64_t)(row * rows));
    return res;
}

/* An unaligned producer plane copied on the device into an aligned plane of
 * the frame's own (VMAFX_IMPORT_ALLOW_COPY). */
static CUresult copy_plane(const VmafxCudaDevice *dev, CUdeviceptr src, size_t src_pitch,
                           CUdeviceptr dst, size_t dst_pitch, size_t row, size_t rows)
{
    CUDA_MEMCPY2D m = {.srcMemoryType = CU_MEMORYTYPE_DEVICE,
                       .srcDevice = src,
                       .srcPitch = src_pitch,
                       .dstMemoryType = CU_MEMORYTYPE_DEVICE,
                       .dstDevice = dst,
                       .dstPitch = dst_pitch,
                       .WidthInBytes = row,
                       .Height = rows};
    return dev->state.f->cuMemcpy2DAsync(&m, dev->state.str);
}

static CUdeviceptr plane_pointer(const VmafxImportPlane *p)
{
    return (CUdeviceptr)p->handle + (CUdeviceptr)p->offset;
}

/* Frame plane `i` from device pointers: bound, converted or (planted) staged. */
static CUresult fill_pointer_plane(const VmafxCudaDevice *dev, const CudaSource *src,
                                   const CudaPlan *plan, CUdeviceptr base, uint32_t i,
                                   CUdeviceptr data[3])
{
    const VmafxImportLayout *const layout = src->layout;
    const bool pair = layout->interleaved && i > 0u;
    const VmafxImportPlane *const p = &src->d->plane[pair ? 1u : i];
    data[i] = plan->owned[i] ? base + plan->offset[i] : plane_pointer(p);
    if (!plan->owned[i] || (i == 2u && pair)) {
        return CUDA_SUCCESS; /* bound, or written with plane 1 */
    }
    if (pair) {
        return deinterleave(dev, plan, base, plane_pointer(p), (size_t)p->pitch, layout->shift);
    }
    if (layout->shift != 0u) {
        return shift_luma(dev, plan, base, plane_pointer(p), (size_t)p->pitch, layout->shift);
    }
    if (plan->staged[i]) {
        return stage_through_host(dev, plane_pointer(p), (size_t)p->pitch, data[i], plan->pitch[i],
                                  (size_t)plan->pw[i] * plan->bytes, plan->ph[i]);
    }
    assert(plan->copied[i]);
    return copy_plane(dev, plane_pointer(p), (size_t)p->pitch, data[i], plan->pitch[i],
                      (size_t)plan->pw[i] * plan->bytes, plan->ph[i]);
}

/* One array plane copied (device to device) into linear memory at `dst`. */
static CUresult copy_array(const VmafxCudaDevice *dev, CUarray array, CUdeviceptr dst,
                           size_t dst_pitch, size_t row, size_t rows)
{
    CUDA_MEMCPY2D m = {.srcMemoryType = CU_MEMORYTYPE_ARRAY,
                       .srcArray = array,
                       .dstMemoryType = CU_MEMORYTYPE_DEVICE,
                       .dstDevice = dst,
                       .dstPitch = dst_pitch,
                       .WidthInBytes = row,
                       .Height = rows};
    return dev->state.f->cuMemcpy2DAsync(&m, dev->state.str);
}

/* Frame plane `i` out of arrays: the luma and planar planes read out, the
 * P010 luma shifted in place, the interleaved chroma read into the staging
 * rows and de-interleaved. */
static CUresult fill_array_plane(const VmafxCudaDevice *dev, const CudaSource *src,
                                 const CudaPlan *plan, CUdeviceptr base, uint32_t i,
                                 CUdeviceptr data[3])
{
    const VmafxImportLayout *const layout = src->layout;
    data[i] = base + plan->offset[i];
    if (layout->interleaved && i == 2u) {
        return CUDA_SUCCESS; /* written with plane 1 */
    }
    if (layout->interleaved && i == 1u) {
        const CUdeviceptr staging = base + plan->staging;
        const CUresult res = copy_array(dev, src->arrays[1], staging, plan->staging_pitch,
                                        2u * (size_t)plan->pw[1] * plan->bytes, plan->ph[1]);
        return res == CUDA_SUCCESS ?
                   deinterleave(dev, plan, base, staging, plan->staging_pitch, layout->shift) :
                   res;
    }
    const CUresult res = copy_array(dev, src->arrays[i], data[i], plan->pitch[i],
                                    (size_t)plan->pw[i] * plan->bytes, plan->ph[i]);
    if (res != CUDA_SUCCESS || layout->shift == 0u) {
        return res;
    }
    return shift_luma(dev, plan, base, data[i], plan->pitch[i], layout->shift);
}

/* An array's geometry matches the plane it holds. */
static VmafxStatus check_array(const VmafxReport *report, const VmafxCudaDevice *dev,
                               const CudaSource *src, const CudaPlan *plan, uint32_t i)
{
    CUDA_ARRAY3D_DESCRIPTOR a;
    memset(&a, 0, sizeof(a));
    const CUresult res = dev->state.f->cuArray3DGetDescriptor(&a, src->arrays[i]);
    const unsigned channels = src->layout->interleaved && i == 1u ? 2u : 1u;
    const CUarray_format format =
        plan->bytes == 1u ? CU_AD_FORMAT_UNSIGNED_INT8 : CU_AD_FORMAT_UNSIGNED_INT16;
    if (res == CUDA_SUCCESS && a.Width == plan->pw[i] && a.Height == plan->ph[i] && a.Depth == 0u &&
        a.Format == format && a.NumChannels == channels) {
        return VMAFX_OK;
    }
    return VMAFX_FAIL(report, res == CUDA_SUCCESS ? VMAFX_E_INVALID : VMAFX_E_DEVICE, (int32_t)res,
                      VMAFX_SUBJECT_PLANE, vmafx_import_plane_field(i, "handle"),
                      "plane %u: array %zux%zux%zu, format 0x%x, %u channel(s); the plane is "
                      "%ux%u of %u-bit samples, %u channel(s) (CUDA error %d)",
                      (unsigned)i, a.Width, a.Height, a.Depth, (unsigned)a.Format,
                      (unsigned)a.NumChannels, plan->pw[i], plan->ph[i],
                      plan->bytes == 1u ? 8u : 16u, channels, (int)res);
}

static VmafxStatus check_arrays(const VmafxReport *report, const VmafxCudaDevice *dev,
                                const CudaSource *src, const CudaPlan *plan)
{
    for (uint32_t i = 0; i < src->layout->n_planes; i++) {
        const VmafxStatus status = check_array(report, dev, src, plan, i);
        if (status != VMAFX_OK) {
            return status;
        }
    }
    return VMAFX_OK;
}

/* The frame's own planes, stream-ordered on the library stream. */
static VmafxStatus alloc_owned(const VmafxReport *report, VmafxCudaFrame *cf, const CudaPlan *plan)
{
    if (plan->total == 0u) {
        return VMAFX_OK;
    }
    const CUresult res =
        cf->dev->state.f->cuMemAllocAsync(&cf->owned, plan->total, cf->dev->state.str);
    if (res == CUDA_SUCCESS) {
        return VMAFX_OK;
    }
    cf->owned = 0;
    return VMAFX_FAIL(report,
                      (int)res == VMAFX_CU_ERROR_OUT_OF_MEMORY ? VMAFX_E_NOMEM : VMAFX_E_DEVICE,
                      (int32_t)res, VMAFX_SUBJECT_FRAME, "frame",
                      "backend cuda: cannot allocate %zu bytes of converted planes (CUDA error "
                      "%d)",
                      plan->total, (int)res);
}

/* Log once per device that VMAFX_IMPORT_ALLOW_COPY made a device copy. */
static void note_device_copy(VmafxCudaDevice *dev, const VmafxFrameImport *d)
{
    if (atomic_exchange(&dev->copy_logged, 1u) == 0u) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "vmafx: backend cuda device %d: VMAFX_IMPORT_ALLOW_COPY: the planes of %s "
                 "imports are copied on the device (no host copy)\n",
                 (int)dev->index, vmafx_memory_kind_name(d->memory));
    }
}

/* Every plane of the frame into `data` / `stride`, the work on the library
 * stream behind the acquire wait. */
static VmafxStatus fill_planes(const VmafxReport *report, VmafxCudaFrame *cf, const CudaSource *src,
                               void *data[3], ptrdiff_t stride[3])
{
    CudaPlan plan;
    const bool force_copy = !src->from_arrays && vmafx_test_switch(VMAFX_TEST_FORCE_HOST_COPY);
    plan_planes(src, force_copy, &plan);
    VmafxStatus status = src->from_arrays ? check_arrays(report, cf->dev, src, &plan) : VMAFX_OK;
    if (status == VMAFX_OK) {
        status = alloc_owned(report, cf, &plan);
    }
    CUresult res = CUDA_SUCCESS;
    CUdeviceptr planes[3] = {0, 0, 0};
    for (uint32_t i = 0; i < plan.n_out && status == VMAFX_OK && res == CUDA_SUCCESS; i++) {
        res = src->from_arrays ? fill_array_plane(cf->dev, src, &plan, cf->owned, i, planes) :
                                 fill_pointer_plane(cf->dev, src, &plan, cf->owned, i, planes);
        /* NOLINTNEXTLINE(performance-no-int-to-ptr): device addresses travel in VmafPicture.data as void *, as vmaf_cuda_picture_alloc() stores them (ADR-2023). */
        data[i] = (void *)planes[i];
        stride[i] = (ptrdiff_t)(plan.owned[i] ? plan.pitch[i] : src->d->plane[i].pitch);
    }
    if (status == VMAFX_OK && res != CUDA_SUCCESS) {
        status = VMAFX_FAIL(report, VMAFX_E_DEVICE, (int32_t)res, VMAFX_SUBJECT_FRAME, "frame",
                            "backend cuda: cannot enqueue the planes of the import (CUDA error %d)",
                            (int)res);
    }
    if (status == VMAFX_OK && src->layout->interleaved) {
        vmafx_count_conversion();
    }
    if (status == VMAFX_OK && (plan.copied[0] || plan.copied[1] || plan.copied[2] ||
                               (src->from_arrays && !src->layout->interleaved))) {
        note_device_copy(cf->dev, src->d);
    }
    return status;
}

/* ---- Binding ------------------------------------------------------------------ */

int vmafx_cuda_picture_attach(VmafxCudaDevice *dev, VmafPicture *pic, bool ordered)
{
    VmafPicturePrivate *const priv = pic->priv;
    assert(priv != NULL && dev->state.str != NULL);
    CudaFunctions *const f = dev->state.f;
    priv->buf_type = VMAF_PICTURE_BUFFER_TYPE_CUDA_DEVICE;
    priv->cuda.ctx = dev->state.ctx;
    priv->cuda.str = dev->state.str;
    priv->cuda.state = &dev->state;
    priv->cuda.ordered = ordered;
    priv->cuda.vmafx = true;
    CUresult res = f->cuEventCreate(&priv->cuda.ready, CU_EVENT_DISABLE_TIMING);
    if (res == CUDA_SUCCESS) {
        res = f->cuEventCreate(&priv->cuda.finished, CU_EVENT_DISABLE_TIMING);
    }
    if (res == CUDA_SUCCESS) {
        res = f->cuEventRecord(priv->cuda.ready, dev->state.str);
    }
    if (res == CUDA_SUCCESS) {
        res = f->cuEventRecord(priv->cuda.finished, dev->state.str);
    }
    if (res != CUDA_SUCCESS) {
        vmafx_cuda_picture_detach(dev, pic);
    }
    return res == CUDA_SUCCESS ? 0 : vmaf_cuda_result_to_errno((int)res);
}

void vmafx_cuda_picture_detach(VmafxCudaDevice *dev, VmafPicture *pic)
{
    VmafPicturePrivate *const priv = pic->priv;
    if (priv->cuda.ready) {
        (void)dev->state.f->cuEventDestroy(priv->cuda.ready);
        priv->cuda.ready = NULL;
    }
    if (priv->cuda.finished) {
        (void)dev->state.f->cuEventDestroy(priv->cuda.finished);
        priv->cuda.finished = NULL;
    }
}

int vmafx_cuda_frame_release(VmafxFrame *frame, VmafPicture *pic)
{
    VmafxCudaFrame *const cf = frame->lane;
    assert(cf != NULL && cf->dev != NULL && pic != NULL);
    VmafxCudaDevice *const dev = cf->dev;
    VmafxHostFence *const fence = atomic_exchange(&frame->released, (VmafxHostFence *)NULL);
    frame->lane = NULL;
    int err = vmafx_cuda_push(dev);
    if (!err) {
        if (frame->pool) {
            vmafx_cuda_pool_frame_released(frame, dev->state.str);
        }
        err = vmafx_cuda_gl_release(cf);
        const int freed = vmafx_cuda_release_frame(cf, fence);
        err = err ? err : freed;
        vmafx_cuda_picture_detach(dev, pic);
        err = vmafx_cuda_pop(dev, err);
    } else {
        /* No context: the frame's state cannot be freed on the device. */
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vmafx: backend cuda: cannot release a frame (%d)\n", err);
    }
    return err;
}

/* The acquire wait on the library stream (none under the planted defect). */
static VmafxStatus wait_acquire(const VmafxReport *report, const VmafxCudaDevice *dev,
                                const VmafxFence *acquire)
{
    if (acquire->kind != VMAFX_FENCE_CUDA_EVENT ||
        vmafx_test_switch(VMAFX_TEST_SKIP_ACQUIRE_WAIT)) {
        return VMAFX_OK;
    }
    /* NOLINTNEXTLINE(performance-no-int-to-ptr): the producer's CUevent crosses the ABI as uintptr_t (VmafxFence.handle, ADR-1929). */
    CUevent event = (CUevent)acquire->handle;
    const CUresult res = dev->state.f->cuStreamWaitEvent(dev->state.str, event, 0);
    if (res == CUDA_SUCCESS) {
        return VMAFX_OK;
    }
    return VMAFX_FAIL(report, VMAFX_E_INVALID, (int32_t)res, VMAFX_SUBJECT_FENCE,
                      "desc.acquire.handle",
                      "backend cuda: cannot wait on the acquire event 0x%llx (CUDA error %d); is "
                      "it an event of the device's context?",
                      (unsigned long long)acquire->handle, (int)res);
}

/* The planes behind the acquire fence, with the context pushed. */
static VmafxStatus enqueue_import(const VmafxReport *report, VmafxCudaFrame *cf, CudaSource *src,
                                  void *data[3], ptrdiff_t stride[3])
{
    const VmafxFrameImport *const d = src->d;
    VmafxStatus status = VMAFX_OK;
    if (d->memory == VMAFX_MEMORY_GL_TEXTURE) {
        status = vmafx_cuda_gl_map(report, cf, d, src->arrays);
    } else if (d->memory == VMAFX_MEMORY_DEVICE_ARRAY) {
        for (uint32_t i = 0; i < src->layout->n_planes; i++) {
            /* NOLINTNEXTLINE(performance-no-int-to-ptr): the producer's CUarray crosses the ABI as uintptr_t (VmafxImportPlane.handle, ADR-1929). */
            src->arrays[i] = (CUarray)d->plane[i].handle;
        }
    }
    if (status == VMAFX_OK) {
        status = wait_acquire(report, cf->dev, &d->acquire);
    }
    return status == VMAFX_OK ? fill_planes(report, cf, src, data, stride) : status;
}

/* The VmafxFrame over the planes, a CUDA picture read on the library stream. */
static VmafxStatus bind_picture(const VmafxReport *report, VmafxCudaFrame *cf,
                                const CudaSource *src, void *data[3], ptrdiff_t stride[3],
                                VmafxFrame *frame)
{
    VmafxFrameDesc planar = VMAFX_FRAME_DESC_INIT;
    planar.pix_fmt = src->layout->planar_fmt;
    planar.bpc = src->d->bpc;
    planar.w = src->d->w;
    planar.h = src->d->h;
    int err = vmafx_frame_bind(frame, &planar, data, stride);
    if (!err) {
        err = vmafx_cuda_picture_attach(cf->dev, &frame->pic, true);
        if (err) {
            (void)vmaf_ref_close(frame->pic.ref);
            free(frame->pic.priv);
            frame->pic.priv = NULL;
        }
    }
    if (err) {
        return VMAFX_FAIL(report, err == -ENOMEM ? VMAFX_E_NOMEM : VMAFX_E_DEVICE, err,
                          VMAFX_SUBJECT_FRAME, "frame", "backend cuda: cannot bind the frame (%d)",
                          err);
    }
    return VMAFX_OK;
}

/* Bind a checked descriptor: lane state, planes, picture. */
static VmafxStatus bind_cuda_frame(const VmafxReport *report, VmafxDevice *device, CudaSource *src,
                                   VmafxFrame **out)
{
    VmafxCudaDevice *const dev = vmafx_cuda_dev(device);
    VmafxCudaFrame *const cf = vmafx_cuda_frame_state_new(dev);
    VmafxFrame *const frame = cf ? calloc(1, sizeof(*frame)) : NULL;
    if (!frame || vmafx_cuda_push(dev) != 0) {
        free(frame);
        vmafx_cuda_frame_state_free(cf);
        return VMAFX_FAIL(report, frame ? VMAFX_E_DEVICE : VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_FRAME,
                          "frame", "backend cuda: cannot start the import");
    }
    void *data[3] = {NULL, NULL, NULL};
    ptrdiff_t stride[3] = {0, 0, 0};
    VmafxStatus status = enqueue_import(report, cf, src, data, stride);
    if (status == VMAFX_OK) {
        status = bind_picture(report, cf, src, data, stride, frame);
    }
    if (status != VMAFX_OK) {
        /* Whatever was enqueued finishes before the stream-ordered free. */
        (void)vmafx_cuda_gl_release(cf);
        (void)vmafx_cuda_release_frame(cf, NULL);
        (void)vmafx_cuda_pop(dev, 0);
        free(frame);
        return status;
    }
    (void)vmafx_cuda_pop(dev, 0);
    assert(frame->pic.priv != NULL && frame->pic.ref != NULL);
    frame->lane = cf;
    frame->lane_release = vmafx_cuda_frame_release;
    frame->release = src->d->release;
    frame->user = src->d->user;
    frame->residency = VMAFX_BACKEND_CUDA;
    frame->device = vmafx_device_ref(device);
    *out = frame;
    return VMAFX_OK;
}

VmafxStatus vmafx_cuda_frame_import(const VmafxReport *report, VmafxDevice *device,
                                    const VmafxFrameImport *desc, const VmafxImportLayout *layout,
                                    VmafxFrame **out)
{
    VmafxStatus status = check_cuda_memory(report, desc, layout);
    if (status == VMAFX_OK) {
        status = check_cuda_planes(report, desc, layout);
    }
    if (status == VMAFX_OK) {
        status = check_cuda_acquire(report, desc);
    }
    if (status != VMAFX_OK) {
        return status;
    }
    CudaSource src = {.d = desc,
                      .layout = layout,
                      .arrays = {NULL, NULL, NULL},
                      .from_arrays = desc->memory != VMAFX_MEMORY_DEVICE_POINTER};
    return bind_cuda_frame(report, device, &src, out);
}

/* NOLINTEND(modernize-use-nullptr) */
