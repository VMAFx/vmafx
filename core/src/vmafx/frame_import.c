/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Imported frames of the VMAFx API (RC4 WP3 common lane, ADR-1852 design
 * section 2.7, ADR-1829): vmafx_frame_import() and vmafx_frame_release_fence().
 *
 * The CPU device binds host memory: planar planes are used where they are
 * (no copy), NV12 / P010 / P016 are planarised into planes of the frame's own
 * (a de-interleave, and for P010 the shift of ADR-1679; nothing else), with
 * the row readers the Metal import uses (metal/iosurface_layout.h), so a
 * converted frame scores bit for bit as the same frame created on the host.
 * The device backends bind their memory kinds behind the same function in
 * their lanes; until then a device or memory kind the build cannot bind is
 * refused naming it, never copied through the host.
 *
 * An imported frame follows the frame-reference rule of ADR-1906 item 4: one
 * reference is one count of its picture's VmafRef, so a frame imported once
 * is scored by several contexts without a second import or copy, and its
 * release fence is signalled where the last count is dropped
 * (vmafx_frame_release() in frame_host.c).
 *
 * The CPU device has no queue to put a wait on: an acquire fence that is not
 * signalled yet is VMAFX_E_BUSY, which vmafx_context_import_frame() answers
 * with a host wait and one retry (decision D8).
 */

#include <assert.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "error_internal.h"
#include "frame_import_hooks.h"
#ifdef HAVE_CUDA
#include "cuda/vmafx_cuda.h"
#endif
#include "internal.h"
#include "mem.h"
#include "metal/iosurface_layout.h"
#include "picture.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Largest luma dimension the engine allocates (picture.c VMAF_PIC_DIM_MAX);
 * above the 16K x 16K the API promises (the format envelope, PR #2185). */
#define VMAFX_IMPORT_DIM_MAX 32768u
/* Row alignment of the planes an import converts (picture.c DATA_ALIGN). */
#define VMAFX_IMPORT_ALIGN 64u

typedef VmafxImportLayout ImportLayout;

static const ImportLayout import_layouts[] = {
    {VMAFX_PIXEL_FORMAT_YUV420P, VMAFX_PIXEL_FORMAT_YUV420P, 3u, 8u, 16u, 0u, false, "yuv420p"},
    {VMAFX_PIXEL_FORMAT_YUV422P, VMAFX_PIXEL_FORMAT_YUV422P, 3u, 8u, 16u, 0u, false, "yuv422p"},
    {VMAFX_PIXEL_FORMAT_YUV444P, VMAFX_PIXEL_FORMAT_YUV444P, 3u, 8u, 16u, 0u, false, "yuv444p"},
    {VMAFX_PIXEL_FORMAT_YUV400P, VMAFX_PIXEL_FORMAT_YUV400P, 1u, 8u, 16u, 0u, false, "gray"},
    {VMAFX_PIXEL_FORMAT_NV12, VMAFX_PIXEL_FORMAT_YUV420P, 2u, 8u, 8u, 0u, true, "nv12"},
    {VMAFX_PIXEL_FORMAT_P010, VMAFX_PIXEL_FORMAT_YUV420P, 2u, 10u, 10u, 6u, true, "p010"},
    {VMAFX_PIXEL_FORMAT_P016, VMAFX_PIXEL_FORMAT_YUV420P, 2u, 16u, 16u, 0u, true, "p016"},
};

#define N_IMPORT_LAYOUTS (sizeof(import_layouts) / sizeof(import_layouts[0]))

static const char *const memory_names[] = {"NONE",          "HOST",         "DEVICE_POINTER",
                                           "DEVICE_ARRAY",  "DMABUF",       "METAL_SURFACE",
                                           "METAL_TEXTURE", "WIN32_SHARED", "GL_TEXTURE"};

const char *vmafx_memory_kind_name(uint32_t memory)
{
    return memory < sizeof(memory_names) / sizeof(memory_names[0]) ? memory_names[memory] :
                                                                     "unknown";
}

static const ImportLayout *import_layout(uint32_t pix_fmt)
{
    for (size_t i = 0; i < N_IMPORT_LAYOUTS; i++) {
        if (import_layouts[i].pix_fmt == pix_fmt) {
            return &import_layouts[i];
        }
    }
    return NULL;
}

const char *vmafx_import_format_name(uint32_t pix_fmt)
{
    const ImportLayout *const layout = import_layout(pix_fmt);
    return layout ? layout->name : "unknown";
}

/* ---- Checks -------------------------------------------------------------------- */

static const char *const plane_handle_names[] = {"desc.plane[0].handle", "desc.plane[1].handle",
                                                 "desc.plane[2].handle"};
static const char *const plane_pitch_names[] = {"desc.plane[0].pitch", "desc.plane[1].pitch",
                                                "desc.plane[2].pitch"};
static const char *const plane_modifier_names[] = {
    "desc.plane[0].modifier", "desc.plane[1].modifier", "desc.plane[2].modifier"};
static const char *const plane_size_names[] = {"desc.plane[0].size", "desc.plane[1].size",
                                               "desc.plane[2].size"};
static const char *const plane_index_names[] = {
    "desc.plane[0].plane_index", "desc.plane[1].plane_index", "desc.plane[2].plane_index"};
static const char *const plane_offset_names[] = {"desc.plane[0].offset", "desc.plane[1].offset",
                                                 "desc.plane[2].offset"};

const char *vmafx_import_plane_field(uint32_t i, const char *field)
{
    assert(i < 3u && field != NULL);
    const char *const *const names = strcmp(field, "handle") == 0   ? plane_handle_names :
                                     strcmp(field, "pitch") == 0    ? plane_pitch_names :
                                     strcmp(field, "modifier") == 0 ? plane_modifier_names :
                                     strcmp(field, "size") == 0     ? plane_size_names :
                                     strcmp(field, "offset") == 0   ? plane_offset_names :
                                                                      plane_index_names;
    return names[i];
}

static bool dim_ok(uint32_t dim)
{
    return dim != 0u && dim <= VMAFX_IMPORT_DIM_MAX;
}

/* Width and height: 1 to VMAFX_IMPORT_DIM_MAX. */
static VmafxStatus check_size(const VmafxReport *report, const VmafxFrameImport *d)
{
    if (dim_ok(d->w) && dim_ok(d->h)) {
        return VMAFX_OK;
    }
    return VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_PARAMETER,
                      dim_ok(d->w) ? "desc.h" : "desc.w", "%ux%u: a frame is 1x1 to %ux%u",
                      (unsigned)d->w, (unsigned)d->h, VMAFX_IMPORT_DIM_MAX, VMAFX_IMPORT_DIM_MAX);
}

/* The geometry fields of an import descriptor. */
static VmafxStatus check_geometry(const VmafxReport *report, const VmafxFrameImport *d,
                                  const ImportLayout **layout)
{
    *layout = import_layout(d->pix_fmt);
    if (!*layout) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.pix_fmt",
                          "pixel format %u is not one vmafx_frame_import() takes",
                          (unsigned)d->pix_fmt);
    }
    if (d->bpc < (*layout)->bpc_min || d->bpc > (*layout)->bpc_max) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.bpc",
                          "%u bits per component; %s holds %u to %u", (unsigned)d->bpc,
                          (*layout)->name, (unsigned)(*layout)->bpc_min,
                          (unsigned)(*layout)->bpc_max);
    }
    const VmafxStatus size = check_size(report, d);
    if (size != VMAFX_OK) {
        return size;
    }
    if (d->n_planes != (*layout)->n_planes) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.n_planes",
                          "%s has %u planes, not %u", (*layout)->name,
                          (unsigned)(*layout)->n_planes, (unsigned)d->n_planes);
    }
    if (d->flags & ~(uint32_t)VMAFX_IMPORT_ALLOW_COPY) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.flags",
                          "unknown flag bits 0x%x",
                          (unsigned)(d->flags & ~VMAFX_IMPORT_ALLOW_COPY));
    }
    return VMAFX_OK;
}

/* A value of VmafxMemoryKind other than NONE. */
static VmafxStatus check_memory_kind(const VmafxReport *report, const VmafxFrameImport *d)
{
    if (d->memory == VMAFX_MEMORY_NONE || d->memory > VMAFX_MEMORY_GL_TEXTURE) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                          "memory kind %u is not a VmafxMemoryKind", (unsigned)d->memory);
    }
    return VMAFX_OK;
}

/* The CPU device binds the descriptor's memory kind. */
static VmafxStatus check_memory(const VmafxReport *report, const VmafxFrameImport *d,
                                const ImportLayout *layout)
{
    if (d->memory != VMAFX_MEMORY_HOST) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                          "backend cpu, memory %s, pixel format %s: the CPU device binds HOST "
                          "memory only, and a host copy of device memory is never made",
                          vmafx_memory_kind_name(d->memory), layout->name);
    }
    return VMAFX_OK;
}

void vmafx_import_plane_extent(const ImportLayout *layout, uint32_t bpc, uint32_t i,
                               const unsigned pw[3], const unsigned ph[3], uint64_t *row,
                               uint64_t *rows)
{
    const uint64_t bytes = bpc > 8u ? 2u : 1u;
    const uint64_t pair = layout->interleaved && i == 1u ? 2u : 1u;
    *row = (uint64_t)pw[i] * bytes * pair;
    *rows = ph[i];
}

/* The rows of a plane (`rows` >= 1 of `row` bytes, pitch >= row > 0) lie
 * inside the address space and, when the producer gave one, its size. */
static bool plane_fits(const VmafxImportPlane *p, uint64_t row, uint64_t rows)
{
    if ((rows - 1u) > (UINT64_MAX - row) / p->pitch) {
        return false;
    }
    const uint64_t extent = (rows - 1u) * p->pitch + row;
    const uint64_t room = (uint64_t)UINTPTR_MAX - (uint64_t)p->handle;
    if (p->offset > room || extent > room - p->offset) {
        return false;
    }
    return p->size == 0u || (p->offset <= p->size && extent <= p->size - p->offset);
}

VmafxStatus vmafx_import_check_linear_plane(const VmafxReport *report, const VmafxImportPlane *p,
                                            uint32_t i, uint64_t row, uint64_t rows,
                                            const char *memory)
{
    if (p->handle == 0u) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PLANE, plane_handle_names[i],
                          "plane %u has no address", (unsigned)i);
    }
    if (p->modifier != 0u) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PLANE, plane_modifier_names[i],
                          "plane %u: modifier 0x%llx; %s memory is read linearly (modifier 0) "
                          "and is never de-tiled through a copy",
                          (unsigned)i, (unsigned long long)p->modifier, memory);
    }
    if (p->pitch < row || p->pitch > (uint64_t)PTRDIFF_MAX) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PLANE, plane_pitch_names[i],
                          "plane %u: pitch %llu cannot hold a row of %llu bytes", (unsigned)i,
                          (unsigned long long)p->pitch, (unsigned long long)row);
    }
    if (!plane_fits(p, row, rows)) {
        return VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_PLANE, plane_size_names[i],
                          "plane %u: %llu rows of pitch %llu from offset %llu do not fit in %llu "
                          "bytes",
                          (unsigned)i, (unsigned long long)rows, (unsigned long long)p->pitch,
                          (unsigned long long)p->offset, (unsigned long long)p->size);
    }
    return VMAFX_OK;
}

static VmafxStatus check_planes(const VmafxReport *report, const VmafxFrameImport *d,
                                const ImportLayout *layout)
{
    unsigned pw[3];
    unsigned ph[3];
    vmaf_picture_plane_extents(vmafx_engine_pixel_format(layout->planar_fmt), d->w, d->h, pw, ph);
    for (uint32_t i = 0; i < layout->n_planes; i++) {
        uint64_t row = 0;
        uint64_t rows = 0;
        vmafx_import_plane_extent(layout, d->bpc, i, pw, ph, &row, &rows);
        const VmafxStatus status =
            vmafx_import_check_linear_plane(report, &d->plane[i], i, row, rows, "host");
        if (status != VMAFX_OK) {
            return status;
        }
    }
    return VMAFX_OK;
}

/* The acquire fence is one the CPU device can honour and, unless a test
 * planted the missing wait, already signalled. */
static VmafxStatus check_acquire(const VmafxReport *report, const VmafxFence *acquire)
{
    if (acquire->kind == VMAFX_FENCE_NONE) {
        return VMAFX_OK;
    }
    if (acquire->kind != VMAFX_FENCE_HOST) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_FENCE, "desc.acquire.kind",
                          "backend cpu: an acquire fence of kind %u; the CPU device waits on "
                          "NONE and HOST fences",
                          (unsigned)acquire->kind);
    }
    VmafxHostFence *host = NULL;
    const VmafxStatus status = vmafx_host_fence_of(report, acquire, "desc.acquire.handle", &host);
    if (status != VMAFX_OK || vmafx_host_fence_signalled(host) ||
        vmafx_test_switch(VMAFX_TEST_SKIP_ACQUIRE_WAIT)) {
        return status;
    }
    return VMAFX_FAIL(report, VMAFX_E_BUSY, 0, VMAFX_SUBJECT_FENCE, "desc.acquire",
                      "the producer has not signalled the acquire fence; the CPU device cannot "
                      "queue a wait (vmafx_context_import_frame() waits and retries once)");
}

/* ---- Binding -------------------------------------------------------------------- */

/* First sample of producer plane `i`. */
static uint8_t *plane_address(const VmafxImportPlane *p)
{
    /* SAFETY: vmafx_import_check_linear_plane() proved handle + offset + the plane's extent
     * <= UINTPTR_MAX and, when the producer gave a size, inside it; the
     * producer guarantees the memory is readable until the release fence. */
    /* NOLINTNEXTLINE(performance-no-int-to-ptr): a host plane crosses the ABI as uintptr_t (ADR-1852 design section 2.1 item 6, ADR-1929). */
    return (uint8_t *)(p->handle + (uintptr_t)p->offset);
}

/* Row stride of a plane the import allocates. */
static size_t owned_stride(unsigned w, uint32_t bpc)
{
    const size_t row = (size_t)w * (bpc > 8u ? 2u : 1u);
    return (row + VMAFX_IMPORT_ALIGN - 1u) & ~(size_t)(VMAFX_IMPORT_ALIGN - 1u);
}

/* Where each plane of the frame comes from: SIZE_MAX for a producer plane
 * bound as it is, else its offset in the allocation of `*total` bytes. A
 * plane is converted when the layout interleaves or shifts it, or when the
 * planted host-copy defect copies every plane. */
static void plan_planes(const VmafxFrameImport *d, const ImportLayout *layout, bool copy_all,
                        const unsigned pw[3], const unsigned ph[3], size_t offsets[3],
                        size_t *total)
{
    const uint32_t n_out = layout->planar_fmt == VMAFX_PIXEL_FORMAT_YUV400P ? 1u : 3u;
    *total = 0;
    for (uint32_t i = 0; i < 3u; i++) {
        const bool converted = copy_all || layout->shift != 0u || (i > 0u && layout->interleaved);
        offsets[i] = i < n_out && converted ? *total : SIZE_MAX;
        *total += offsets[i] == SIZE_MAX ? 0u : owned_stride(pw[i], d->bpc) * ph[i];
    }
}

/* Plane `i` of the frame: the producer's plane (`dst` NULL), or its
 * conversion into `dst`. */
static void fill_plane(const VmafxFrameImport *d, const ImportLayout *layout, uint32_t i,
                       const unsigned pw[3], const unsigned ph[3], uint8_t *dst, void *data[3],
                       ptrdiff_t stride[3])
{
    const bool pair = layout->interleaved && i > 0u;
    const VmafxImportPlane *const p = &d->plane[pair ? 1u : i];
    if (!dst) {
        data[i] = plane_address(p);
        stride[i] = (ptrdiff_t)p->pitch;
        return;
    }
    const VmafMetalPlaneRead rd = {.src_plane = pair ? 1u : i,
                                   .step = pair ? 2u : 1u,
                                   .offset = pair ? i - 1u : 0u,
                                   .bytes = d->bpc > 8u ? 2u : 1u,
                                   .shift = layout->shift};
    data[i] = dst;
    stride[i] = (ptrdiff_t)owned_stride(pw[i], d->bpc);
    vmaf_metal_read_plane(dst, (size_t)stride[i], plane_address(p), (size_t)p->pitch, pw[i], ph[i],
                          &rd);
}

/* Fill data / stride for every plane of the frame, converting into one
 * aligned allocation `*owned` of `*bytes` bytes (NULL / 0 when every plane
 * is bound as it is): 0, or -1 without memory. */
static int convert_planes(const VmafxFrameImport *d, const ImportLayout *layout, bool copy_all,
                          void *data[3], ptrdiff_t stride[3], void **owned, size_t *bytes)
{
    unsigned pw[3];
    unsigned ph[3];
    vmaf_picture_plane_extents(vmafx_engine_pixel_format(layout->planar_fmt), d->w, d->h, pw, ph);
    size_t offsets[3];
    plan_planes(d, layout, copy_all, pw, ph, offsets, bytes);
    uint8_t *const buffer = *bytes ? aligned_malloc(*bytes, VMAFX_IMPORT_ALIGN) : NULL;
    if (*bytes && !buffer) {
        return -1;
    }
    const uint32_t n_out = layout->planar_fmt == VMAFX_PIXEL_FORMAT_YUV400P ? 1u : 3u;
    for (uint32_t i = 0; i < n_out; i++) {
        /* A converted plane has an offset only when there is an allocation. */
        assert(offsets[i] == SIZE_MAX || buffer != NULL);
        fill_plane(d, layout, i, pw, ph, offsets[i] == SIZE_MAX ? NULL : buffer + offsets[i], data,
                   stride);
    }
    *owned = buffer;
    return 0;
}

/* The frame over the checked descriptor. */
static VmafxStatus bind_frame(const VmafxReport *report, VmafxDevice *device,
                              const VmafxFrameImport *d, const ImportLayout *layout,
                              VmafxFrame **out)
{
    const bool copy_all = vmafx_test_switch(VMAFX_TEST_FORCE_HOST_COPY);
    void *data[3] = {NULL, NULL, NULL};
    ptrdiff_t stride[3] = {0, 0, 0};
    void *owned = NULL;
    size_t bytes = 0;
    VmafxFrame *const frame = calloc(1, sizeof(*frame));
    if (!frame || convert_planes(d, layout, copy_all, data, stride, &owned, &bytes) != 0) {
        free(frame);
        return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_FRAME, "frame",
                          "cannot allocate a %ux%u frame", (unsigned)d->w, (unsigned)d->h);
    }
    VmafxFrameDesc planar = VMAFX_FRAME_DESC_INIT;
    planar.pix_fmt = layout->planar_fmt;
    planar.bpc = d->bpc;
    planar.w = d->w;
    planar.h = d->h;
    if (vmafx_frame_bind(frame, &planar, data, stride) != 0) {
        aligned_free(owned);
        free(frame);
        return VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_FRAME, "frame",
                          "cannot allocate a frame");
    }
    if (owned && copy_all) {
        vmafx_count_host_copy((uint64_t)bytes);
    } else if (owned) {
        vmafx_count_conversion();
    }
    const uint32_t residency = vmafx_test_import_residency();
    frame->residency = residency == VMAFX_TEST_RESIDENCY_OFF ? device->backend : residency;
    frame->owned = owned;
    frame->release = d->release;
    frame->user = d->user;
    frame->device = vmafx_device_ref(device);
    *out = frame;
    return VMAFX_OK;
}

/* The CPU device's checks and binding of a geometry-checked descriptor. */
static VmafxStatus import_on_cpu(const VmafxReport *report, VmafxDevice *device,
                                 const VmafxFrameImport *d, const ImportLayout *layout,
                                 VmafxFrame **out)
{
    VmafxStatus status = check_memory(report, d, layout);
    if (status == VMAFX_OK) {
        status = check_planes(report, d, layout);
    }
    if (status == VMAFX_OK) {
        status = check_acquire(report, &d->acquire);
    }
    return status == VMAFX_OK ? bind_frame(report, device, d, layout, out) : status;
}

/* The import on the lane of the device's backend. */
static VmafxStatus import_on_device(const VmafxReport *report, VmafxDevice *device,
                                    const VmafxFrameImport *d, const ImportLayout *layout,
                                    VmafxFrame **out)
{
    switch (device->backend) {
    case VMAFX_BACKEND_CPU:
        return import_on_cpu(report, device, d, layout, out);
#ifdef HAVE_CUDA
    case VMAFX_BACKEND_CUDA:
        return vmafx_cuda_frame_import(report, device, d, layout, out);
#endif
    default:
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_BACKEND, "device",
                          "backend %s, memory %s, pixel format %s: this build imports on the "
                          "devices of the backends it was built with",
                          vmafx_backend_name(device->backend), vmafx_memory_kind_name(d->memory),
                          layout->name);
    }
}

VmafxStatus vmafx_frame_import(VmafxDevice *device, const VmafxFrameImport *desc, VmafxFrame **out,
                               VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (out) {
        *out = NULL;
    }
    if (!out || !desc) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !out ? "out" : "desc", "NULL argument");
    }
    vmafx_count_import_attempt();
    const VmafxStatus planted = vmafx_test_take_import_failure();
    if (planted != VMAFX_OK) {
        return VMAFX_FAIL(&report, planted, 0, VMAFX_SUBJECT_FRAME, "desc",
                          "planted import failure (test switch)");
    }
    VmafxDevice *const dev = device ? device : vmafx_device_cpu();
    VmafxFrameImport d = VMAFX_FRAME_IMPORT_INIT;
    const ImportLayout *layout = NULL;
    VmafxStatus status =
        vmafx_read_sized(&report, &d, (uint32_t)sizeof(d), desc, VMAFX_MIN_FRAME_IMPORT, "desc");
    if (status == VMAFX_OK) {
        status = check_geometry(&report, &d, &layout);
    }
    if (status == VMAFX_OK) {
        status = check_memory_kind(&report, &d);
    }
    return status == VMAFX_OK ? import_on_device(&report, dev, &d, layout, out) : status;
}

/* ---- Release fences ---------------------------------------------------------------- */

/* The frame's release fence, created on first use (set once, atomically). */
static VmafxHostFence *release_fence_of(VmafxFrame *frame)
{
    VmafxHostFence *const current = atomic_load(&frame->released);
    if (current) {
        return current;
    }
    VmafxHostFence *const fresh = vmafx_host_fence_new();
    if (!fresh) {
        return NULL;
    }
    VmafxHostFence *expected = NULL;
    if (atomic_compare_exchange_strong(&frame->released, &expected, fresh)) {
        return fresh;
    }
    vmafx_host_fence_unref(fresh);
    return expected;
}

VmafxStatus vmafx_frame_release_fence(VmafxFrame *frame, uint32_t kind, VmafxFence *out,
                                      VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!frame || !out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          !frame ? "frame" : "out", "NULL argument");
    }
#ifdef HAVE_CUDA
    if (kind != VMAFX_FENCE_HOST && kind != VMAFX_FENCE_NONE && frame->lane &&
        frame->device->backend == VMAFX_BACKEND_CUDA) {
        return vmafx_cuda_release_fence(&report, frame, kind, out);
    }
#endif
    if (kind != VMAFX_FENCE_HOST) {
        return VMAFX_FAIL(&report, kind == VMAFX_FENCE_NONE ? VMAFX_E_INVALID : VMAFX_E_NOTSUP, 0,
                          VMAFX_SUBJECT_FENCE, "kind",
                          "backend %s: a release fence of kind %u; the frames of this device "
                          "signal HOST release fences",
                          vmafx_backend_name(frame->device->backend), (unsigned)kind);
    }
    VmafxHostFence *const fence = release_fence_of(frame);
    if (!fence) {
        return VMAFX_FAIL(&report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_FENCE, "out",
                          "cannot allocate a host fence");
    }
    VmafxFence full = VMAFX_FENCE_INIT;
    full.kind = VMAFX_FENCE_HOST;
    full.handle = (uintptr_t)vmafx_host_fence_ref(fence);
    const VmafxStatus status =
        vmafx_write_sized(&report, out, &full, (uint32_t)sizeof(full), "out");
    if (status != VMAFX_OK) {
        vmafx_host_fence_unref(fence);
    }
    return status;
}

/* NOLINTEND(modernize-use-nullptr) */
