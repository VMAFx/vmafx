/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The VMAFx device-frame API on a SYCL device (RC4 WP3, ADR-1929, ADR-2091):
 * devices (by index and from the caller's queue), USM imports of every
 * layout and USM kind, dma-buf imports (linear from a Level Zero export,
 * Intel-tiled from a VA-API surface), refusals naming the field, admission,
 * the planted host copy, pools, and one import scored by two contexts with
 * its release fences signalled after the last reader of either.
 *
 * Needs a Level Zero GPU (77 without one); the tiled dma-buf case needs VA-API
 * on it and is skipped without.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/frame_import_hooks.h"
#include "vmafx/internal.h"
#include "sycl/vmafx_sycl_internal.h"
#include "sycl/vmafx_sycl_rt.h"
#include "vmafx/sync_object.h"
#include "vmafx/vmafx.h"
#include "vmafx_import_test_util.h"
#include "vmafx_sycl_cells.h"
#include "vmafx_sycl_test_util.h"
#include "vmafx_test_util.h"

#ifdef VMAFX_TEST_HAVE_VA
#include <fcntl.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define W 352u
#define H 288u
#define FRAMES 4u
#define MODEL "vmaf_v1.0.16_3d0h"
/* The psnr cell of vmafx_sycl_cells.h. */
#define CELL_PSNR (&vs_cells[17])

static VsGpu gpu;
static bool have_gpu;

/* Two frames per index of deterministic 4:2:0 content. */
typedef struct Clip {
    VmafxFrameDesc desc;
    uint8_t *ref;
    uint8_t *dist;
} Clip;

static bool clip_make(Clip *c, uint32_t bpc)
{
    c->desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, bpc, W, H);
    const size_t frame = vt_frame_bytes(&c->desc);
    c->ref = malloc(frame * FRAMES);
    c->dist = malloc(frame * FRAMES);
    for (unsigned i = 0; i < FRAMES && c->ref && c->dist; i++) {
        vt_fill(&c->desc, c->ref + i * frame, i);
        vt_fill(&c->desc, c->dist + i * frame, i + 5u);
    }
    return c->ref && c->dist;
}

static void clip_free(Clip *c)
{
    free(c->ref);
    free(c->dist);
}

/* The frames every test scores, made once. */
static Clip clip8;
static Clip clip10;

/* ---- Devices ------------------------------------------------------------------------ */

static char *test_device_info(void)
{
    uint32_t count = 0;
    mu_assert("count",
              vmafx_device_count(VMAFX_BACKEND_SYCL, &count, NULL) == VMAFX_OK && count >= 1u);
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    mu_assert("info", vmafx_device_info(VMAFX_BACKEND_SYCL, 0, &info, NULL) == VMAFX_OK);
    const uint32_t fences = (1u << VMAFX_FENCE_NONE) | (1u << VMAFX_FENCE_HOST) |
                            (1u << VMAFX_FENCE_SYCL_EVENT) | (1u << VMAFX_FENCE_SYNC_FILE) |
                            (1u << VMAFX_FENCE_GL_SYNC);
    const uint32_t memory = (1u << VMAFX_MEMORY_DEVICE_POINTER) | (1u << VMAFX_MEMORY_DMABUF) |
                            (1u << VMAFX_MEMORY_GL_TEXTURE);
    mu_assert("fields", info.backend == VMAFX_BACKEND_SYCL && info.index == 0 && info.flags == 0u &&
                            info.total_memory > 0u && info.name && info.name[0] != '\0' &&
                            info.fence_kinds == fences && info.memory_kinds == memory);
    VmafxError *error = NULL;
    mu_assert("past the last",
              vmafx_device_info(VMAFX_BACKEND_SYCL, (int32_t)count, &info, &error) ==
                      VMAFX_E_NOTFOUND &&
                  vt_failed(&error, VMAFX_E_NOTFOUND, "index", VMAFX_SUBJECT_DEVICE));
    VmafxDeviceInfo described = VMAFX_DEVICE_INFO_INIT;
    mu_assert("describe", vmafx_device_describe(gpu.device, &described, NULL) == VMAFX_OK &&
                              described.backend == VMAFX_BACKEND_SYCL && described.index == 0 &&
                              !strcmp(described.name, info.name));
    return NULL;
}

static char *test_device_create_refusals(void)
{
    VmafxError *error = NULL;
    VmafxDevice *device = NULL;
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.backend = VMAFX_BACKEND_SYCL;
    desc.flags = VMAFX_DEVICE_PROFILING;
    mu_assert("profiling",
              vmafx_device_create(&desc, &device, &error) == VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "desc.flags", VMAFX_SUBJECT_PARAMETER));
    desc.flags = 0u;
    desc.external[1] = 1u;
    mu_assert("second handle",
              vmafx_device_create(&desc, &device, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "desc.external[1]", VMAFX_SUBJECT_PARAMETER));
    desc.external[1] = 0u;
    desc.index = 99;
    mu_assert("no device 99",
              vmafx_device_create(&desc, &device, &error) == VMAFX_E_NOTFOUND &&
                  vt_failed(&error, VMAFX_E_NOTFOUND, "desc.index", VMAFX_SUBJECT_DEVICE));
    mu_assert("nothing made", device == NULL);
    return NULL;
}

/* A device from the caller's queue: the library queue lives in its
 * context, so the caller's USM imports there. */
static char *test_device_external(void)
{
    VsGpu ext;
    mu_assert("open from the producer's queue", vs_open_gpu(&ext, true));
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    const bool described = vmafx_device_describe(ext.device, &info, NULL) == VMAFX_OK &&
                           info.backend == VMAFX_BACKEND_SYCL && info.index == -1;
    VsPlanes p;
    memset(&p, 0, sizeof(p));
    const bool up =
        vs_upload(&ext, &clip8.desc, clip8.ref, VMAFX_PIXEL_FORMAT_NV12, 0u, 0u, 0u, &p);
    const VmafxFrameImport imp = vs_import_desc(&clip8.desc, VMAFX_PIXEL_FORMAT_NV12, 8u, &p);
    VmafxFrame *frame = NULL;
    const bool imported = up && vmafx_frame_import(ext.device, &imp, &frame, NULL) == VMAFX_OK;
    vmafx_frame_unref(frame);
    VmafxContext *const context = vc_cell_context(ext.device, CELL_PSNR);
    const bool on_it = context && vmafx_context_destroy(context, NULL) == VMAFX_OK;
    vs_free_planes(&ext, &p);
    vs_close_gpu(&ext);
    mu_assert("described", described);
    mu_assert("imports the caller's USM", imported);
    mu_assert("context on it", on_it);
    return NULL;
}

/* The context picks SYCL twins once the device is attached. */
static char *test_context_device(void)
{
    VmafxContext *context = NULL;
    mu_assert("context", vmafx_context_create(NULL, &context, NULL) == VMAFX_OK);
    mu_assert("use device", vmafx_context_use_device(context, gpu.device, NULL) == VMAFX_OK);
    VmafxError *error = NULL;
    mu_assert("one device", vmafx_context_use_device(context, gpu.device, &error) == VMAFX_E_BUSY &&
                                vt_failed(&error, VMAFX_E_BUSY, "device", VMAFX_SUBJECT_DEVICE));
    VmafxFeatureResolution r = VMAFX_FEATURE_RESOLUTION_INIT;
    mu_assert("twin", vmafx_feature_resolve(context, "adm", NULL, NULL, &r, NULL) == VMAFX_OK &&
                          r.backend == VMAFX_BACKEND_SYCL && strstr(r.extractor, "sycl"));
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* ---- Refusals ------------------------------------------------------------------------ */

typedef struct Refusal {
    const char *what;
    void (*mutate)(VmafxFrameImport *imp);
    const char *subject;
    VmafxStatus status;
    uint32_t kind;
} Refusal;

static uint8_t foreign[64];

static void m_host(VmafxFrameImport *imp)
{
    imp->memory = VMAFX_MEMORY_HOST;
}

static void m_array(VmafxFrameImport *imp)
{
    imp->memory = VMAFX_MEMORY_DEVICE_ARRAY;
}

static void m_win32(VmafxFrameImport *imp)
{
    imp->memory = VMAFX_MEMORY_WIN32_SHARED;
}

static void m_modifier(VmafxFrameImport *imp)
{
    imp->plane[1].modifier = 0x100000000000002ull;
}

static void m_cuda_event(VmafxFrameImport *imp)
{
    imp->acquire.kind = VMAFX_FENCE_CUDA_EVENT;
    imp->acquire.handle = 1u;
}

static void m_gl_sync_on_pointers(VmafxFrameImport *imp)
{
    imp->acquire.kind = VMAFX_FENCE_GL_SYNC;
    imp->acquire.handle = 1u;
}

static void m_event_without_handle(VmafxFrameImport *imp)
{
    imp->acquire.kind = VMAFX_FENCE_SYCL_EVENT;
}

static void m_bad_sync_file(VmafxFrameImport *imp)
{
    imp->acquire.kind = VMAFX_FENCE_SYNC_FILE;
    imp->acquire.fd = -1;
}

static void m_null_plane(VmafxFrameImport *imp)
{
    imp->plane[1].handle = 0u;
    imp->plane[1].offset = 0u;
}

static void m_short_pitch(VmafxFrameImport *imp)
{
    imp->plane[0].pitch = W - 1u;
}

static void m_foreign_pointer(VmafxFrameImport *imp)
{
    imp->plane[0].handle = (uintptr_t)foreign;
    imp->plane[0].offset = 0u;
    imp->plane[0].pitch = W;
    imp->plane[0].size = 0u;
    imp->h = 1u;
}

static const Refusal refusals[] = {
    {"host memory", m_host, "desc.memory", VMAFX_E_NOTSUP, VMAFX_SUBJECT_PARAMETER},
    {"device array", m_array, "desc.memory", VMAFX_E_NOTSUP, VMAFX_SUBJECT_PARAMETER},
    {"Windows shared", m_win32, "desc.memory", VMAFX_E_NOTSUP, VMAFX_SUBJECT_PARAMETER},
    {"modifier on USM", m_modifier, "desc.plane[1].modifier", VMAFX_E_NOTSUP, VMAFX_SUBJECT_PLANE},
    {"CUDA event", m_cuda_event, "desc.acquire.kind", VMAFX_E_NOTSUP, VMAFX_SUBJECT_FENCE},
    {"GL sync on pointers", m_gl_sync_on_pointers, "desc.acquire.kind", VMAFX_E_INVALID,
     VMAFX_SUBJECT_FENCE},
    {"event handle", m_event_without_handle, "desc.acquire.handle", VMAFX_E_INVALID,
     VMAFX_SUBJECT_FENCE},
    {"sync_file descriptor", m_bad_sync_file, "desc.acquire.fd", VMAFX_E_INVALID,
     VMAFX_SUBJECT_FENCE},
    {"plane address", m_null_plane, "desc.plane[1].handle", VMAFX_E_INVALID, VMAFX_SUBJECT_PLANE},
    {"pitch", m_short_pitch, "desc.plane[0].pitch", VMAFX_E_INVALID, VMAFX_SUBJECT_PLANE},
    {"foreign pointer", m_foreign_pointer, "desc.plane[0].handle", VMAFX_E_NOTSUP,
     VMAFX_SUBJECT_PLANE},
};

static char *refuse(const VmafxFrameImport *base, const Refusal *r)
{
    VmafxFrameImport imp = *base;
    r->mutate(&imp);
    VmafxError *error = NULL;
    VmafxFrame *frame = NULL;
    const VmafxStatus status = vmafx_frame_import(gpu.device, &imp, &frame, &error);
    const bool named = status == r->status && vt_failed(&error, r->status, r->subject, r->kind);
    if (!named) {
        (void)fprintf(stderr, "\n  refusal %s: status %d\n", r->what, (int)status);
    }
    vmafx_frame_unref(frame);
    mu_assert("refused, the field named", named && frame == NULL);
    return NULL;
}

static char *test_import_refusals(void)
{
    VsPlanes p;
    memset(&p, 0, sizeof(p));
    mu_assert("upload",
              vs_upload(&gpu, &clip8.desc, clip8.ref, VMAFX_PIXEL_FORMAT_NV12, 0u, 64u, 0u, &p));
    const VmafxFrameImport nv12 = vs_import_desc(&clip8.desc, VMAFX_PIXEL_FORMAT_NV12, 8u, &p);
    char *msg = NULL;
    for (size_t i = 0; i < sizeof(refusals) / sizeof(refusals[0]) && !msg; i++) {
        msg = refuse(&nv12, &refusals[i]);
    }
    vs_free_planes(&gpu, &p);
    return msg;
}

/* An unsignalled HOST acquire fence: the SYCL device answers VMAFX_E_BUSY
 * (vmafx_context_import_frame() waits on the host and retries). */
static char *test_host_acquire_busy(void)
{
    VsPlanes p;
    memset(&p, 0, sizeof(p));
    mu_assert("upload",
              vs_upload(&gpu, &clip8.desc, clip8.ref, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u, 0u, &p));
    VmafxFrameImport imp = vs_import_desc(&clip8.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &p);
    mu_assert("fence",
              vmafx_fence_create(gpu.device, VMAFX_FENCE_HOST, &imp.acquire, NULL) == VMAFX_OK);
    VmafxError *error = NULL;
    VmafxFrame *frame = NULL;
    const VmafxStatus busy = vmafx_frame_import(gpu.device, &imp, &frame, &error);
    const bool named = vt_failed(&error, VMAFX_E_BUSY, "desc.acquire", VMAFX_SUBJECT_FENCE);
    mu_assert("signal", vmafx_fence_signal(&imp.acquire, NULL) == VMAFX_OK);
    const VmafxStatus imported = vmafx_frame_import(gpu.device, &imp, &frame, NULL);
    vmafx_frame_unref(frame);
    (void)vmafx_fence_destroy(&imp.acquire, NULL);
    vs_free_planes(&gpu, &p);
    mu_assert("busy", busy == VMAFX_E_BUSY && named);
    mu_assert("imported once signalled", imported == VMAFX_OK);
    return NULL;
}

/* ---- Sessions ---------------------------------------------------------------------- */

static VmafxContext *model_context(VmafxModel *model)
{
    VmafxContext *context = NULL;
    if (vmafx_context_create(NULL, &context, NULL) != VMAFX_OK) {
        return NULL;
    }
    if (vmafx_context_use_device(context, gpu.device, NULL) != VMAFX_OK ||
        vmafx_context_use_model(context, model, NULL) != VMAFX_OK) {
        (void)vmafx_context_destroy(context, NULL);
        return NULL;
    }
    return context;
}

/* The host session of `c` on a context with `model` on the SYCL device. */
static VmafxContext *run_host(const Clip *c, VmafxModel *model)
{
    VmafxContext *const context = model_context(model);
    const size_t frame = vt_frame_bytes(&c->desc);
    bool ok = context != NULL;
    for (unsigned i = 0; i < FRAMES && ok; i++) {
        VmafxFrame *ref = vt_wrap_frame(&c->desc, c->ref + i * frame, NULL);
        VmafxFrame *dist = vt_wrap_frame(&c->desc, c->dist + i * frame, NULL);
        ok = vmafx_submit(context, ref, dist, i, NULL) == VMAFX_OK;
    }
    return ok && vmafx_flush(context, NULL) == VMAFX_OK ? context : NULL;
}

/* `frames[2 * i]`, `frames[2 * i + 1]`: reference and distorted of frame i. */
static char *submit_frames(VmafxContext *context, VmafxFrame *const *frames)
{
    for (unsigned i = 0; i < FRAMES; i++) {
        VmafxError *error = NULL;
        const VmafxStatus status =
            vmafx_submit(context, vmafx_frame_ref(frames[(size_t)2u * i]),
                         vmafx_frame_ref(frames[(size_t)2u * i + 1u]), i, &error);
        if (status != VMAFX_OK) {
            (void)fprintf(stderr, "\n  submit: %s\n", vmafx_error_message(error));
        }
        vmafx_error_free(error);
        mu_assert("submit", status == VMAFX_OK);
    }
    mu_assert("flush", vmafx_flush(context, NULL) == VMAFX_OK);
    return NULL;
}

/* A context with `model` scoring `frames`, compared with the host session. */
static char *score_and_compare(const Clip *c, VmafxModel *model, VmafxFrame *const *frames)
{
    VmafxContext *const imp = model_context(model);
    char *msg = imp ? submit_frames(imp, frames) : "context";
    VmafxContext *const host = msg ? NULL : run_host(c, model);
    unsigned long compared = 0;
    unsigned long differing = 0;
    msg = msg ? msg :
                (host && vc_compare(host, imp, FRAMES, &compared, &differing) ? NULL : "compare");
    (void)fprintf(stderr, "[%lu values, %lu differing] ", compared, differing);
    msg = msg ? msg : (differing == 0u ? NULL : "bit-identical");
    if (imp) {
        (void)vmafx_context_destroy(imp, NULL);
    }
    if (host) {
        (void)vmafx_context_destroy(host, NULL);
    }
    return msg;
}

/* Every frame of `c` uploaded as `pix_fmt` into USM from `alloc` (with
 * `pad` and `skew`), imported and scored with `model` as the host frames. */
typedef void *(*Alloc)(VsProducer *p, size_t bytes);

static char *usm_case(const Clip *c, VmafxModel *model, uint32_t pix_fmt, Alloc alloc, size_t pad,
                      size_t skew)
{
    VsPlanes planes[2u * FRAMES];
    VmafxFrame *frames[2u * FRAMES];
    memset(planes, 0, sizeof(planes));
    memset((void *)frames, 0, sizeof(frames));
    const size_t frame = vt_frame_bytes(&c->desc);
    const unsigned shift = pix_fmt == VMAFX_PIXEL_FORMAT_P010 ? 6u : 0u;
    bool ok = true;
    for (unsigned k = 0; k < 2u * FRAMES && ok; k++) {
        const uint8_t *const src = (k & 1u ? c->dist : c->ref) + (k / 2u) * frame;
        size_t rows[3];
        size_t row_bytes[3];
        vs_layout(&c->desc, pix_fmt, pad, skew, &planes[k], rows, row_bytes);
        planes[k].base = alloc(gpu.producer, vs_span(&planes[k], rows));
        ok = planes[k].base &&
             vs_write(&gpu, &c->desc, src, pix_fmt, shift, &planes[k], rows, row_bytes);
        const VmafxFrameImport imp = vs_import_desc(&c->desc, pix_fmt, c->desc.bpc, &planes[k]);
        ok = ok && vmafx_frame_import(gpu.device, &imp, &frames[k], NULL) == VMAFX_OK;
    }
    char *const msg = ok ? score_and_compare(c, model, frames) : "import";
    for (unsigned k = 0; k < 2u * FRAMES; k++) {
        vmafx_frame_unref(frames[k]);
    }
    for (unsigned k = 0; k < 2u * FRAMES; k++) {
        vs_free_planes(&gpu, &planes[k]);
    }
    return msg;
}

/* USM of every kind (device, shared, host) and any layout (an odd start, an
 * odd pitch) is bound where it is; NV12 / P010 are planarised on the device.
 * Every case scores as the host frames, with no host copy. */
static char *test_usm_layouts(void)
{
    VmafxModel *model = NULL;
    mu_assert("model", vmafx_model_load(NULL, MODEL, &model, NULL) == VMAFX_OK);
    char *msg = usm_case(&clip8, model, VMAFX_PIXEL_FORMAT_YUV420P, vs_alloc, 3u, 5u);
    msg = msg ? msg : usm_case(&clip8, model, VMAFX_PIXEL_FORMAT_NV12, vs_alloc, 1u, 1u);
    msg = msg ? msg : usm_case(&clip8, model, VMAFX_PIXEL_FORMAT_YUV420P, vs_alloc_shared, 0u, 0u);
    msg = msg ? msg : usm_case(&clip8, model, VMAFX_PIXEL_FORMAT_NV12, vs_alloc_host, 16u, 0u);
    msg = msg ? msg : usm_case(&clip10, model, VMAFX_PIXEL_FORMAT_P010, vs_alloc, 6u, 2u);
    msg = msg ? msg : usm_case(&clip10, model, VMAFX_PIXEL_FORMAT_YUV420P, vs_alloc, 0u, 2u);
    vmafx_model_unref(model);
    mu_assert_msg(msg);
    mu_assert("no host copy", vmafx_test_host_copies() == 0u);
    return NULL;
}

/* ---- dma-bufs ------------------------------------------------------------------------- */

/* The descriptor of a linear dma-buf over `p` (offsets from the allocation's
 * start, which the dma-buf exports). */
static VmafxFrameImport dmabuf_desc(const Clip *c, uint32_t pix_fmt, const VsPlanes *p, int fd,
                                    uint64_t size)
{
    VmafxFrameImport imp = vs_import_desc(&c->desc, pix_fmt, c->desc.bpc, p);
    imp.memory = VMAFX_MEMORY_DMABUF;
    for (uint32_t i = 0; i < p->n && i < 3u; i++) {
        imp.plane[i].handle = 0u;
        imp.plane[i].fd = fd;
        imp.plane[i].size = size;
    }
    return imp;
}

typedef struct LinearBuf {
    VsPlanes planes;
    int fd;
    uint64_t size;
} LinearBuf;

/* One frame of `c` in a Level Zero allocation exported as a dma-buf. */
static bool linear_frame(const Clip *c, const uint8_t *src, uint32_t pix_fmt, LinearBuf *b)
{
    size_t rows[3];
    size_t row_bytes[3];
    vs_layout(&c->desc, pix_fmt, 32u, 0u, &b->planes, rows, row_bytes);
    b->planes.base = vs_alloc_dmabuf(gpu.producer, vs_span(&b->planes, rows), &b->fd, &b->size);
    return b->planes.base &&
           vs_write(&gpu, &c->desc, src, pix_fmt, 0u, &b->planes, rows, row_bytes);
}

static void linear_free(LinearBuf *b)
{
    if (b->fd >= 0) {
        (void)close(b->fd);
    }
    vs_free_planes(&gpu, &b->planes);
}

/* Linear dma-bufs are bound where they are; refusals name the plane. */
static char *test_dmabuf_linear(void)
{
    VmafxModel *model = NULL;
    mu_assert("model", vmafx_model_load(NULL, MODEL, &model, NULL) == VMAFX_OK);
    LinearBuf bufs[2u * FRAMES];
    VmafxFrame *frames[2u * FRAMES];
    memset(bufs, 0, sizeof(bufs));
    memset((void *)frames, 0, sizeof(frames));
    const size_t frame = vt_frame_bytes(&clip8.desc);
    bool ok = true;
    for (unsigned k = 0; k < 2u * FRAMES; k++) {
        bufs[k].fd = -1;
    }
    for (unsigned k = 0; k < 2u * FRAMES && ok; k++) {
        const uint8_t *const src = (k & 1u ? clip8.dist : clip8.ref) + (k / 2u) * frame;
        ok = linear_frame(&clip8, src, VMAFX_PIXEL_FORMAT_NV12, &bufs[k]);
        const VmafxFrameImport imp =
            dmabuf_desc(&clip8, VMAFX_PIXEL_FORMAT_NV12, &bufs[k].planes, bufs[k].fd, bufs[k].size);
        ok = ok && vmafx_frame_import(gpu.device, &imp, &frames[k], NULL) == VMAFX_OK;
    }
    char *msg = ok ? score_and_compare(&clip8, model, frames) : "dma-buf import";
    VmafxFrameImport bad =
        dmabuf_desc(&clip8, VMAFX_PIXEL_FORMAT_NV12, &bufs[0].planes, bufs[0].fd, bufs[0].size);
    VmafxError *error = NULL;
    VmafxFrame *refused = NULL;
    bad.plane[1].modifier = 0x0100000000000001ull; /* X-tiled */
    const bool x_tiled =
        vmafx_frame_import(gpu.device, &bad, &refused, &error) == VMAFX_E_NOTSUP &&
        vt_failed(&error, VMAFX_E_NOTSUP, "desc.plane[1].modifier", VMAFX_SUBJECT_PLANE);
    bad.plane[1].modifier = 0u;
    bad.plane[0].size = 0u;
    const bool no_size =
        vmafx_frame_import(gpu.device, &bad, &refused, &error) == VMAFX_E_INVALID &&
        vt_failed(&error, VMAFX_E_INVALID, "desc.plane[0].size", VMAFX_SUBJECT_PLANE);
    bad.plane[0].size = bufs[0].size;
    bad.plane[1].offset = bufs[0].size;
    const bool outside =
        vmafx_frame_import(gpu.device, &bad, &refused, &error) == VMAFX_E_RANGE &&
        vt_failed(&error, VMAFX_E_RANGE, "desc.plane[1].size", VMAFX_SUBJECT_PLANE);
    for (unsigned k = 0; k < 2u * FRAMES; k++) {
        vmafx_frame_unref(frames[k]);
        linear_free(&bufs[k]);
    }
    vmafx_model_unref(model);
    mu_assert_msg(msg);
    mu_assert("X-tiled named", x_tiled && refused == NULL);
    mu_assert("missing size named", no_size);
    mu_assert("plane outside the dma-buf named", outside);
    return NULL;
}

/* A sync_file acquire fence the producer already signalled (the dma-buf's
 * own fences exported, which a finished writer leaves signalled) is passed;
 * vmafx_fence_wait() polls a sync_file too. */
static char *test_sync_file_acquire(void)
{
    LinearBuf b;
    memset(&b, 0, sizeof(b));
    b.fd = -1;
    mu_assert("dma-buf", linear_frame(&clip8, clip8.ref, VMAFX_PIXEL_FORMAT_NV12, &b));
    VmafxFrameImport imp = dmabuf_desc(&clip8, VMAFX_PIXEL_FORMAT_NV12, &b.planes, b.fd, b.size);
    const int sync_file = vmafx_dmabuf_export_read_fence(b.fd);
    imp.acquire.kind = VMAFX_FENCE_SYNC_FILE;
    imp.acquire.fd = sync_file;
    VmafxFrame *frame = NULL;
    const bool waited = sync_file >= 0 && vmafx_fence_wait(&imp.acquire, 0u, NULL) == VMAFX_OK;
    const bool imported =
        sync_file >= 0 && vmafx_frame_import(gpu.device, &imp, &frame, NULL) == VMAFX_OK;
    vmafx_frame_unref(frame);
    const bool destroyed = sync_file >= 0 && vmafx_fence_destroy(&imp.acquire, NULL) == VMAFX_OK;
    linear_free(&b);
    mu_assert("a sync_file from the dma-buf", sync_file >= 0);
    mu_assert("signalled", waited);
    mu_assert("imported", imported);
    mu_assert("destroy closes it", destroyed);
    return NULL;
}

#ifdef VMAFX_TEST_HAVE_VA

/* Whether render node `n` is an Intel GPU (its PCI vendor). */
static bool intel_node(int n)
{
    char path[64];
    (void)snprintf(path, sizeof(path), "/sys/class/drm/renderD%d/device/vendor", n);
    FILE *const file = fopen(path, "r");
    char vendor[16] = "";
    const bool read = file && fgets(vendor, sizeof(vendor), file) != NULL;
    if (file) {
        (void)fclose(file);
    }
    return read && strncmp(vendor, "0x8086", 6) == 0;
}

/* A VA-API display on the render node of an Intel GPU, with Intel's media
 * driver named (LIBVA_DRIVER_NAME may name another vendor's), or NULL. */
static VADisplay va_open(int *drm)
{
    for (int n = 128; n < 136; n++) {
        char path[32];
        (void)snprintf(path, sizeof(path), "/dev/dri/renderD%d", n);
        *drm = intel_node(n) ? open(path, O_RDWR | O_CLOEXEC) : -1;
        VADisplay dpy = *drm >= 0 ? vaGetDisplayDRM(*drm) : NULL;
        char driver[] = "iHD";
        int major = 0;
        int minor = 0;
        if (dpy && vaSetDriverName(dpy, driver) == VA_STATUS_SUCCESS &&
            vaInitialize(dpy, &major, &minor) == VA_STATUS_SUCCESS) {
            return dpy;
        }
        if (dpy) {
            (void)vaTerminate(dpy);
        }
        if (*drm >= 0) {
            (void)close(*drm);
        }
    }
    *drm = -1;
    return NULL;
}

/* Write planar `src` into surface `s` as NV12 (8-bit) or P010 (10-bit). */
static bool va_put(VADisplay dpy, VASurfaceID s, const Clip *c, const uint8_t *src)
{
    const bool p010 = c->desc.bpc > 8u;
    VAImageFormat fmt = {.fourcc = p010 ? VA_FOURCC_P010 : VA_FOURCC_NV12,
                         .byte_order = VA_LSB_FIRST,
                         .bits_per_pixel = p010 ? 24u : 12u};
    VAImage img;
    memset(&img, 0, sizeof(img));
    if (vaCreateImage(dpy, &fmt, (int)W, (int)H, &img) != VA_STATUS_SUCCESS) {
        return false;
    }
    uint8_t *map = NULL;
    bool ok = vaMapBuffer(dpy, img.buf, (void **)&map) == VA_STATUS_SUCCESS;
    if (ok) {
        uint8_t *const semi = malloc(vt_frame_bytes(&c->desc));
        ok = semi != NULL;
        if (ok) {
            vt_to_semiplanar(&c->desc, src, p010 ? 6u : 0u, semi);
            const size_t bytes = p010 ? 2u : 1u;
            for (unsigned y = 0; y < H; y++) {
                memcpy(map + img.offsets[0] + (size_t)y * img.pitches[0],
                       semi + (size_t)y * W * bytes, W * bytes);
            }
            const uint8_t *const chroma = semi + (size_t)W * H * bytes;
            for (unsigned y = 0; y < H / 2u; y++) {
                memcpy(map + img.offsets[1] + (size_t)y * img.pitches[1],
                       chroma + (size_t)y * W * bytes, W * bytes);
            }
        }
        free(semi);
        ok = vaUnmapBuffer(dpy, img.buf) == VA_STATUS_SUCCESS && ok;
    }
    ok = ok && vaPutImage(dpy, s, img.image_id, 0, 0, W, H, 0, 0, W, H) == VA_STATUS_SUCCESS &&
         vaSyncSurface(dpy, s) == VA_STATUS_SUCCESS;
    (void)vaDestroyImage(dpy, img.image_id);
    return ok;
}

/* The DMABUF descriptor of an exported surface (one layer, two planes). */
static VmafxFrameImport va_desc(const Clip *c, const VADRMPRIMESurfaceDescriptor *d)
{
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    imp.memory = VMAFX_MEMORY_DMABUF;
    imp.pix_fmt = c->desc.bpc > 8u ? VMAFX_PIXEL_FORMAT_P010 : VMAFX_PIXEL_FORMAT_NV12;
    imp.bpc = c->desc.bpc;
    imp.w = W;
    imp.h = H;
    imp.n_planes = 2u;
    for (uint32_t i = 0; i < 2u; i++) {
        const uint32_t o = d->layers[0].object_index[i];
        imp.plane[i].fd = d->objects[o].fd;
        imp.plane[i].offset = d->layers[0].offset[i];
        imp.plane[i].pitch = d->layers[0].pitch[i];
        imp.plane[i].modifier = d->objects[o].drm_format_modifier;
        imp.plane[i].size = d->objects[o].size;
    }
    return imp;
}

/* Every frame of `c` decoded into tiled VA surfaces (as a decoder leaves
 * them), exported as dma-bufs, imported and scored as the host frames. */
static char *va_case(VADisplay dpy, const Clip *c, VmafxModel *model, uint64_t *modifier)
{
    VASurfaceID surfaces[2u * FRAMES];
    VADRMPRIMESurfaceDescriptor descs[2u * FRAMES];
    VmafxFrame *frames[2u * FRAMES];
    memset((void *)frames, 0, sizeof(frames));
    memset(descs, 0, sizeof(descs));
    const unsigned rt = c->desc.bpc > 8u ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
    bool ok = vaCreateSurfaces(dpy, rt, W, H, surfaces, 2u * FRAMES, NULL, 0) == VA_STATUS_SUCCESS;
    const size_t frame = vt_frame_bytes(&c->desc);
    for (unsigned k = 0; k < 2u * FRAMES && ok; k++) {
        const uint8_t *const src = (k & 1u ? c->dist : c->ref) + (k / 2u) * frame;
        ok = va_put(dpy, surfaces[k], c, src) &&
             vaExportSurfaceHandle(dpy, surfaces[k], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                   &descs[k]) == VA_STATUS_SUCCESS;
        const VmafxFrameImport imp = va_desc(c, &descs[k]);
        *modifier = imp.plane[0].modifier;
        ok = ok && vmafx_frame_import(gpu.device, &imp, &frames[k], NULL) == VMAFX_OK;
    }
    char *const msg = ok ? score_and_compare(c, model, frames) : "VA surface import";
    for (unsigned k = 0; k < 2u * FRAMES; k++) {
        vmafx_frame_unref(frames[k]);
        for (uint32_t o = 0; o < descs[k].num_objects; o++) {
            (void)close(descs[k].objects[o].fd);
        }
    }
    (void)vaDestroySurfaces(dpy, surfaces, 2u * FRAMES);
    return msg;
}

/* ---- A decoder's stream: surfaces reused, one import per frame ---------------------- */

#define STREAM_FRAMES 48u
#define STREAM_SURFACES 4u

/* A context scoring psnr and cambi (the twin whose zero-copy scores
 * diverged under batched command lists, draft PR #2217) on the SYCL device. */
static VmafxContext *stream_context(void)
{
    VmafxContext *context = NULL;
    if (vmafx_context_create(NULL, &context, NULL) != VMAFX_OK) {
        return NULL;
    }
    if (vmafx_context_use_device(context, gpu.device, NULL) != VMAFX_OK ||
        vmafx_context_use_feature(context, "psnr", NULL, NULL) != VMAFX_OK ||
        vmafx_context_use_feature(context, "cambi", NULL, NULL) != VMAFX_OK) {
        (void)vmafx_context_destroy(context, NULL);
        return NULL;
    }
    return context;
}

/* Content of stream frame `i`, side `s`. */
static const uint8_t *stream_src(unsigned i, unsigned s)
{
    return (s ? clip8.dist : clip8.ref) + (i % FRAMES) * vt_frame_bytes(&clip8.desc);
}

/* Write stream frame `i` into the surfaces of its slot (after the release
 * fences of the frames the slot held last), export and import them. */
static bool stream_frame(VADisplay dpy, const VASurfaceID *surfaces, VmafxFence *released,
                         unsigned i, VmafxFrame *pair[2])
{
    bool ok = true;
    for (unsigned s = 0; s < 2u && ok; s++) {
        const unsigned k = 2u * (i % (STREAM_SURFACES / 2u)) + s;
        ok = released[k].kind == VMAFX_FENCE_NONE ||
             vmafx_fence_wait(&released[k], 10000000000ull, NULL) == VMAFX_OK;
        (void)vmafx_fence_destroy(&released[k], NULL);
        released[k] = (VmafxFence)VMAFX_FENCE_INIT;
        VADRMPRIMESurfaceDescriptor desc;
        memset(&desc, 0, sizeof(desc));
        ok = ok && va_put(dpy, surfaces[k], &clip8, stream_src(i, s)) &&
             vaExportSurfaceHandle(dpy, surfaces[k], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                   &desc) == VA_STATUS_SUCCESS;
        const VmafxFrameImport imp = va_desc(&clip8, &desc);
        ok = ok && vmafx_frame_import(gpu.device, &imp, &pair[s], NULL) == VMAFX_OK &&
             vmafx_frame_release_fence(pair[s], VMAFX_FENCE_HOST, &released[k], NULL) == VMAFX_OK;
        for (uint32_t o = 0; o < desc.num_objects; o++) {
            (void)close(desc.objects[o].fd); /* the import holds its own */
        }
    }
    return ok;
}

static VmafxContext *stream_imported(VADisplay dpy)
{
    VASurfaceID surfaces[STREAM_SURFACES];
    VmafxFence released[STREAM_SURFACES];
    for (unsigned k = 0; k < STREAM_SURFACES; k++) {
        released[k] = (VmafxFence)VMAFX_FENCE_INIT;
    }
    VmafxContext *const context = stream_context();
    bool ok = context && vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, W, H, surfaces, STREAM_SURFACES,
                                          NULL, 0) == VA_STATUS_SUCCESS;
    for (unsigned i = 0; i < STREAM_FRAMES && ok; i++) {
        VmafxFrame *pair[2] = {NULL, NULL};
        ok = stream_frame(dpy, surfaces, released, i, pair) &&
             vmafx_submit(context, pair[0], pair[1], i, NULL) == VMAFX_OK;
    }
    ok = ok && vmafx_flush(context, NULL) == VMAFX_OK;
    for (unsigned k = 0; k < STREAM_SURFACES; k++) {
        (void)vmafx_fence_wait(&released[k], 10000000000ull, NULL);
        (void)vmafx_fence_destroy(&released[k], NULL);
    }
    (void)vaDestroySurfaces(dpy, surfaces, STREAM_SURFACES);
    if (!ok && context) {
        (void)vmafx_context_destroy(context, NULL);
    }
    return ok ? context : NULL;
}

static VmafxContext *stream_host(void)
{
    VmafxContext *const context = stream_context();
    bool ok = context != NULL;
    for (unsigned i = 0; i < STREAM_FRAMES && ok; i++) {
        VmafxFrame *ref = vt_wrap_frame(&clip8.desc, (uint8_t *)stream_src(i, 0u), NULL);
        VmafxFrame *dist = vt_wrap_frame(&clip8.desc, (uint8_t *)stream_src(i, 1u), NULL);
        ok = vmafx_submit(context, ref, dist, i, NULL) == VMAFX_OK;
    }
    return ok && vmafx_flush(context, NULL) == VMAFX_OK ? context : NULL;
}

/* A decoder's stream: 48 frames through 4 surfaces, each frame exported and
 * imported once (so each import is freed and the next one may take its
 * address), scored as the host frames. The case of draft PR #2217. */
static char *test_dmabuf_stream(void)
{
    int drm = -1;
    VADisplay dpy = va_open(&drm);
    if (!dpy) {
        (void)fprintf(stderr, "[no VA-API display: skipped] ");
        return NULL;
    }
    VmafxContext *const imported = stream_imported(dpy);
    VmafxContext *const host = imported ? stream_host() : NULL;
    unsigned long compared = 0;
    unsigned long differing = 0;
    const bool same = host && vc_compare(host, imported, STREAM_FRAMES, &compared, &differing);
    (void)fprintf(stderr, "[stream: %lu values, %lu differing] ", compared, differing);
    if (imported) {
        (void)vmafx_context_destroy(imported, NULL);
    }
    if (host) {
        (void)vmafx_context_destroy(host, NULL);
    }
    (void)vaTerminate(dpy);
    (void)close(drm);
    mu_assert("stream scored", same);
    mu_assert("bit-identical", differing == 0u);
    return NULL;
}

/* Intel-tiled dma-bufs (VA surfaces) are de-tiled on the device. */
static char *test_dmabuf_tiled(void)
{
    int drm = -1;
    VADisplay dpy = va_open(&drm);
    if (!dpy) {
        (void)fprintf(stderr, "[no VA-API display: skipped] ");
        return NULL;
    }
    VmafxModel *model = NULL;
    uint64_t mod8 = 0;
    uint64_t mod10 = 0;
    char *msg = vmafx_model_load(NULL, MODEL, &model, NULL) == VMAFX_OK ? NULL : "model";
    msg = msg ? msg : va_case(dpy, &clip8, model, &mod8);
    msg = msg ? msg : va_case(dpy, &clip10, model, &mod10);
    (void)fprintf(stderr, "[VA modifiers: NV12 0x%llx, P010 0x%llx] ", (unsigned long long)mod8,
                  (unsigned long long)mod10);
    vmafx_model_unref(model);
    (void)vaTerminate(dpy);
    (void)close(drm);
    mu_assert_msg(msg);
    mu_assert("de-tiled on the device", vmafx_test_host_copies() == 0u);
    return NULL;
}

#else

static char *test_dmabuf_tiled(void)
{
    (void)fprintf(stderr, "[built without VA-API: skipped] ");
    return NULL;
}

static char *test_dmabuf_stream(void)
{
    (void)fprintf(stderr, "[built without VA-API: skipped] ");
    return NULL;
}

#endif /* VMAFX_TEST_HAVE_VA */

/* ---- Pools -------------------------------------------------------------------------- */

static bool fill_pool_frame(const Clip *c, VmafxFrame *frame, const uint8_t *src)
{
    VmafxFramePlanes p = VMAFX_FRAME_PLANES_INIT;
    unsigned w[3];
    unsigned h[3];
    size_t row[3];
    vt_plane_geometry(&c->desc, w, h, row);
    bool ok = vmafx_frame_planes(frame, &p, NULL) == VMAFX_OK;
    for (uint32_t i = 0; i < p.n_planes && i < 3u && ok; i++) {
        ok = vs_copy_2d(gpu.producer, p.data[i], (size_t)p.stride[i], src, row[i], row[i], h[i]) ==
             0;
        src += row[i] * h[i];
    }
    return ok && vs_finish(gpu.producer) == 0;
}

static char *test_pool(void)
{
    VmafxModel *model = NULL;
    VmafxFramePool *pool = NULL;
    VmafxFrame *frames[2u * FRAMES];
    memset((void *)frames, 0, sizeof(frames));
    mu_assert("model", vmafx_model_load(NULL, MODEL, &model, NULL) == VMAFX_OK);
    mu_assert("pool", vmafx_frame_pool_create(gpu.device, &clip8.desc, 2u * FRAMES, &pool, NULL) ==
                          VMAFX_OK);
    const size_t frame = vt_frame_bytes(&clip8.desc);
    char *msg = NULL;
    for (unsigned k = 0; k < 2u * FRAMES && !msg; k++) {
        const uint8_t *const src = (k & 1u ? clip8.dist : clip8.ref) + (k / 2u) * frame;
        msg = vmafx_frame_pool_acquire(pool, &frames[k], NULL) == VMAFX_OK &&
                      fill_pool_frame(&clip8, frames[k], src) ?
                  NULL :
                  "fill";
    }
    VmafxFrame *extra = NULL;
    VmafxError *error = NULL;
    const bool exhausted = vmafx_frame_pool_acquire(pool, &extra, &error) == VMAFX_E_BUSY &&
                           vt_failed(&error, VMAFX_E_BUSY, "pool", VMAFX_SUBJECT_FRAME);
    msg = msg ? msg : score_and_compare(&clip8, model, frames);
    VmafxFence fence = VMAFX_FENCE_INIT;
    const bool fenced =
        vmafx_frame_release_fence(frames[0], VMAFX_FENCE_SYCL_EVENT, &fence, NULL) == VMAFX_OK;
    for (unsigned k = 0; k < 2u * FRAMES; k++) {
        vmafx_frame_unref(frames[k]);
    }
    const bool released = fenced && vmafx_fence_wait(&fence, 5000000000ull, NULL) == VMAFX_OK;
    (void)vmafx_fence_destroy(&fence, NULL);
    VmafxFrame *again = NULL;
    const bool reused = vmafx_frame_pool_acquire(pool, &again, NULL) == VMAFX_OK;
    vmafx_frame_unref(again);
    vmafx_frame_pool_destroy(pool);
    vmafx_model_unref(model);
    mu_assert_msg(msg);
    mu_assert("exhausted", exhausted);
    mu_assert("pool frame released after its readers", released);
    mu_assert("a frame returns to the pool", reused);
    return NULL;
}

/* ---- Admission ------------------------------------------------------------------------ */

/* An extractor without a SYCL twin runs on the CPU: it would need a host
 * copy of the imported frame and is named. */
static char *test_admission(void)
{
    VsPlanes p;
    memset(&p, 0, sizeof(p));
    mu_assert("upload",
              vs_upload(&gpu, &clip8.desc, clip8.ref, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u, 0u, &p));
    VmafxContext *context = NULL;
    mu_assert("context",
              vmafx_context_create(NULL, &context, NULL) == VMAFX_OK &&
                  vmafx_context_use_device(context, gpu.device, NULL) == VMAFX_OK &&
                  vmafx_context_use_feature(context, "psnr", NULL, NULL) == VMAFX_OK &&
                  vmafx_context_use_feature(context, "delta_e_itp", NULL, NULL) == VMAFX_OK);
    const VmafxFrameImport imp = vs_import_desc(&clip8.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &p);
    VmafxFrame *frame = NULL;
    VmafxError *error = NULL;
    const VmafxStatus status =
        vmafx_context_import_frame(context, gpu.device, &imp, "main", &frame, &error);
    const char *const msg = vmafx_error_message(error);
    const bool named = status == VMAFX_E_NOTSUP && msg && strstr(msg, "delta_e_itp (cpu") &&
                       !strstr(msg, "psnr") && strstr(msg, "main: backend sycl device 0");
    if (!named) {
        (void)fprintf(stderr, "\n  status %d: %s\n", (int)status, msg ? msg : "(none)");
    }
    vmafx_error_free(error);
    (void)vmafx_context_destroy(context, NULL);
    vs_free_planes(&gpu, &p);
    mu_assert("the CPU extractor named, the SYCL twin admitted", named && frame == NULL);
    return NULL;
}

/* ---- One import, two contexts ------------------------------------------------------------ */

typedef struct Shared {
    VmafxModel *models[2];
    VmafxContext *contexts[2];
    VsPlanes planes[2u * FRAMES];
    VmafxFence event; /* SYCL_EVENT release fence of the last reference frame */
    VmafxFence host;  /* HOST release fence of the same frame */
} Shared;

static char *shared_submit(Shared *s)
{
    const size_t frame = vt_frame_bytes(&clip8.desc);
    const uint64_t attempts = vmafx_test_import_attempts();
    for (unsigned i = 0; i < FRAMES; i++) {
        VmafxFrame *pair[2] = {NULL, NULL};
        for (unsigned side = 0; side < 2u; side++) {
            VsPlanes *const p = &s->planes[(size_t)2u * i + side];
            const uint8_t *const src = (side ? clip8.dist : clip8.ref) + i * frame;
            mu_assert("upload",
                      vs_upload(&gpu, &clip8.desc, src, VMAFX_PIXEL_FORMAT_NV12, 0u, 16u, 0u, p));
            VmafxFrameImport imp = vs_import_desc(&clip8.desc, VMAFX_PIXEL_FORMAT_NV12, 8u, p);
            const uintptr_t ev = vs_last_event(gpu.producer);
            imp.acquire = vs_event_fence(ev);
            const VmafxStatus status = vmafx_frame_import(gpu.device, &imp, &pair[side], NULL);
            vs_event_free(ev);
            mu_assert("import", status == VMAFX_OK);
        }
        (void)vmafx_fence_destroy(&s->event, NULL);
        (void)vmafx_fence_destroy(&s->host, NULL);
        mu_assert("fences", vmafx_frame_release_fence(pair[0], VMAFX_FENCE_SYCL_EVENT, &s->event,
                                                      NULL) == VMAFX_OK &&
                                vmafx_frame_release_fence(pair[0], VMAFX_FENCE_HOST, &s->host,
                                                          NULL) == VMAFX_OK);
        for (unsigned k = 0; k < 2u; k++) {
            mu_assert("submit", vmafx_submit(s->contexts[k], vmafx_frame_ref(pair[0]),
                                             vmafx_frame_ref(pair[1]), i, NULL) == VMAFX_OK);
        }
        vmafx_frame_unref(pair[0]);
        vmafx_frame_unref(pair[1]);
    }
    mu_assert("one import per frame",
              vmafx_test_import_attempts() - attempts == 2u * (uint64_t)FRAMES);
    return NULL;
}

/* Each context scores as a run of its own on host frames. */
static char *shared_compare(Shared *s)
{
    for (unsigned k = 0; k < 2u; k++) {
        mu_assert("flush", vmafx_flush(s->contexts[k], NULL) == VMAFX_OK);
        VmafxContext *const alone = run_host(&clip8, s->models[k]);
        unsigned long compared = 0;
        unsigned long differing = 0;
        const bool same = alone && vc_compare(alone, s->contexts[k], FRAMES, &compared, &differing);
        if (alone) {
            (void)vmafx_context_destroy(alone, NULL);
        }
        (void)fprintf(stderr, "[context %u: %lu values, %lu differing%s] ", k, compared, differing,
                      same ? "" : ", collectors differ");
        mu_assert("each context equals its own run", same && differing == 0u);
    }
    return NULL;
}

/* Released after the last reader of either context. */
static char *shared_release(Shared *s)
{
    mu_assert("first", vmafx_context_destroy(s->contexts[0], NULL) == VMAFX_OK);
    s->contexts[0] = NULL;
    mu_assert("the second still holds the last frame",
              vmafx_fence_wait(&s->event, 50000000ull, NULL) == VMAFX_E_TIMEOUT &&
                  vmafx_fence_wait(&s->host, 0u, NULL) == VMAFX_PENDING);
    mu_assert("second", vmafx_context_destroy(s->contexts[1], NULL) == VMAFX_OK);
    s->contexts[1] = NULL;
    mu_assert("released after the last reader",
              vmafx_fence_wait(&s->event, 5000000000ull, NULL) == VMAFX_OK &&
                  vmafx_fence_wait(&s->host, 5000000000ull, NULL) == VMAFX_OK);
    return NULL;
}

static void shared_close(Shared *s)
{
    for (unsigned k = 0; k < 2u; k++) {
        if (s->contexts[k]) {
            (void)vmafx_context_destroy(s->contexts[k], NULL);
        }
        vmafx_model_unref(s->models[k]);
    }
    (void)vmafx_fence_destroy(&s->event, NULL);
    (void)vmafx_fence_destroy(&s->host, NULL);
    for (unsigned k = 0; k < 2u * FRAMES; k++) {
        vs_free_planes(&gpu, &s->planes[k]);
    }
}

static char *test_one_import_two_contexts(void)
{
    Shared s;
    memset(&s, 0, sizeof(s));
    s.event = (VmafxFence)VMAFX_FENCE_INIT;
    s.host = (VmafxFence)VMAFX_FENCE_INIT;
    char *msg = vmafx_model_load(NULL, MODEL, &s.models[0], NULL) == VMAFX_OK &&
                        vmafx_model_load(NULL, "vmaf_v0.6.1", &s.models[1], NULL) == VMAFX_OK ?
                    NULL :
                    "setup";
    s.contexts[0] = msg ? NULL : model_context(s.models[0]);
    s.contexts[1] = msg ? NULL : model_context(s.models[1]);
    msg = msg ? msg : (s.contexts[0] && s.contexts[1] ? NULL : "contexts");
    msg = msg ? msg : shared_submit(&s);
    msg = msg ? msg : shared_compare(&s);
    msg = msg ? msg : shared_release(&s);
    shared_close(&s);
    return msg;
}

/* ---- Host copies ------------------------------------------------------------------------ */

/* The planted host copy (VMAFX_TEST_FORCE_HOST_COPY) is counted, one per
 * plane, and the scores do not change; without it the count stays 0. */
static char *test_host_copy_counter(void)
{
    mu_assert("no host copy so far", vmafx_test_host_copies() == 0u);
    VsPlanes p;
    memset(&p, 0, sizeof(p));
    mu_assert("upload",
              vs_upload(&gpu, &clip8.desc, clip8.ref, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 8u, 0u, &p));
    const VmafxFrameImport imp = vs_import_desc(&clip8.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &p);
    VmafxFrame *frame = NULL;
    vmafx_test_set_switches(VMAFX_TEST_FORCE_HOST_COPY);
    const VmafxStatus status = vmafx_frame_import(gpu.device, &imp, &frame, NULL);
    vmafx_test_set_switches(0u);
    vmafx_frame_unref(frame);
    const uint64_t copies = vmafx_test_host_copies();
    vmafx_test_reset_counters();
    vs_free_planes(&gpu, &p);
    mu_assert("planted", status == VMAFX_OK && copies == 3u);
    return NULL;
}

/* ---- Read recording -------------------------------------------------------------------- */

/* Every engine read of an imported frame is recorded on the frame, so that
 * its release waits on it. On the A380 the device runs the kernels of every
 * queue in submission order, which hides a missing record from the release
 * tests (Research-2159 finding 4); this test counts the records instead. */
static bool reads_session(VmafxContext *context, VsPlanes p[2], VmafxFrame *frames[2],
                          uint32_t reads[2])
{
    bool ok = context != NULL;
    for (unsigned k = 0; k < 2u && ok; k++) {
        ok = vs_upload(&gpu, &clip8.desc, k ? clip8.dist : clip8.ref, VMAFX_PIXEL_FORMAT_YUV420P,
                       0u, 0u, 0u, &p[k]);
        const VmafxFrameImport imp =
            vs_import_desc(&clip8.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &p[k]);
        ok = ok && vmafx_frame_import(gpu.device, &imp, &frames[k], NULL) == VMAFX_OK;
    }
    ok = ok &&
         vmafx_submit(context, vmafx_frame_ref(frames[0]), vmafx_frame_ref(frames[1]), 0u, NULL) ==
             VMAFX_OK &&
         vmafx_flush(context, NULL) == VMAFX_OK;
    for (unsigned k = 0; k < 2u && ok; k++) {
        const VmafxSyclFrame *const sf = frames[k]->lane;
        reads[k] = sf && sf->rt ? vmafx_sycl_rt_frame_reads(sf->rt) : 0u;
    }
    return ok;
}

static char *test_reads_recorded(void)
{
    VmafxModel *model = NULL;
    mu_assert("model", vmafx_model_load(NULL, MODEL, &model, NULL) == VMAFX_OK);
    VmafxContext *const context = model_context(model);
    VsPlanes p[2];
    memset(p, 0, sizeof(p));
    VmafxFrame *frames[2] = {NULL, NULL};
    uint32_t reads[2] = {0u, 0u};
    const bool ok = reads_session(context, p, frames, reads);
    if (context) {
        (void)vmafx_context_destroy(context, NULL);
    }
    for (unsigned k = 0; k < 2u; k++) {
        vmafx_frame_unref(frames[k]);
        vs_free_planes(&gpu, &p[k]);
    }
    vmafx_model_unref(model);
    (void)fprintf(stderr, "[reads recorded: %u reference, %u distorted] ", reads[0], reads[1]);
    mu_assert("session", ok);
    mu_assert("every engine read of an imported frame is recorded", reads[0] > 0u && reads[1] > 0u);
    return NULL;
}

static char *guard(char *(*test)(void))
{
    if (!have_gpu) {
        mu_skipped = 1;
        return NULL;
    }
    return test();
}

#define GUARDED(name)                                                                              \
    static char *name##_g(void)                                                                    \
    {                                                                                              \
        return guard(name);                                                                        \
    }

GUARDED(test_device_info)
GUARDED(test_device_create_refusals)
GUARDED(test_device_external)
GUARDED(test_context_device)
GUARDED(test_import_refusals)
GUARDED(test_host_acquire_busy)
GUARDED(test_usm_layouts)
GUARDED(test_dmabuf_linear)
GUARDED(test_sync_file_acquire)
GUARDED(test_dmabuf_tiled)
GUARDED(test_dmabuf_stream)
GUARDED(test_pool)
GUARDED(test_admission)
GUARDED(test_one_import_two_contexts)
GUARDED(test_host_copy_counter)
GUARDED(test_reads_recorded)

char *run_tests(void)
{
    vmafx_test_reset_counters();
    /* NOLINTNEXTLINE(concurrency-mt-unsafe): single-thread test setup (ADR-0141 / ADR-0278). */
    const char *const batched = getenv("VMAFX_TEST_SYCL_OMIT_IMMEDIATE");
    /* The evaluation of draft PR #2217 (ADR-2091): library queues without the
     * immediate-command-list property, with UR_L0_USE_IMMEDIATE_COMMANDLISTS=0. */
    vmafx_sycl_rt_test_omit_immediate(batched && batched[0] == '1');
    have_gpu = vs_open_gpu(&gpu, false) && clip_make(&clip8, 8u) && clip_make(&clip10, 10u);
    static const MuTest tests[] = {
        MU_TEST(test_device_info_g),       MU_TEST(test_device_create_refusals_g),
        MU_TEST(test_device_external_g),   MU_TEST(test_context_device_g),
        MU_TEST(test_import_refusals_g),   MU_TEST(test_host_acquire_busy_g),
        MU_TEST(test_usm_layouts_g),       MU_TEST(test_dmabuf_linear_g),
        MU_TEST(test_sync_file_acquire_g), MU_TEST(test_dmabuf_tiled_g),
        MU_TEST(test_dmabuf_stream_g),     MU_TEST(test_pool_g),
        MU_TEST(test_admission_g),         MU_TEST(test_one_import_two_contexts_g),
        MU_TEST(test_host_copy_counter_g), MU_TEST(test_reads_recorded_g),
    };
    char *const msg = mu_run_table(tests, MU_TABLE_LEN(tests));
    clip_free(&clip8);
    clip_free(&clip10);
    vs_close_gpu(&gpu);
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
