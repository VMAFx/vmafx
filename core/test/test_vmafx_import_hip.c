/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The VMAFx device-frame API on a HIP device (RC4 WP3, ADR-1929, ADR-2092):
 * devices by index and from the caller's stream, the engine import of a
 * device into a context, every refusal of a HIP import with its named field,
 * device pointers bound at any address and pitch, dma-bufs imported as
 * external memory (with their sync_file as the acquire fence), NV12 / planar
 * imports out of HIP arrays, per-extractor admission, one import scored by
 * two contexts with its release fences signalled after the last reader of
 * either, the release callback, and the host-copy counter (0, and 1 per
 * plane under the planted host copy).
 *
 * Needs a HIP device (77 without one); the dma-buf cases need libgbm at build
 * time and the device's render node at run time.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/frame_import_hooks.h"
#include "vmafx/vmafx.h"
#include "vmafx_device_cells.h"
#include "vmafx_hip_test_util.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define W 352u
#define H 288u
#define FRAMES 4u
#define MODEL "vmaf_v1.0.16_3d0h"

static VhGpu gpu;
static bool have_gpu;
#ifdef VMAFX_TEST_HAVE_GBM
static VhGbm gbm;
static bool have_gbm;
#endif

/* Frames of deterministic 8-bit 4:2:0 content, reference and distorted. */
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

/* ---- Devices ------------------------------------------------------------------------ */

static char *test_device_info(void)
{
    uint32_t count = 0;
    mu_assert("count",
              vmafx_device_count(VMAFX_BACKEND_HIP, &count, NULL) == VMAFX_OK && count >= 1u);
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    mu_assert("info", vmafx_device_info(VMAFX_BACKEND_HIP, 0, &info, NULL) == VMAFX_OK);
    const uint32_t fences = (1u << VMAFX_FENCE_NONE) | (1u << VMAFX_FENCE_HOST) |
                            (1u << VMAFX_FENCE_HIP_EVENT) | (1u << VMAFX_FENCE_GL_SYNC) |
                            (1u << VMAFX_FENCE_SYNC_FILE);
    const uint32_t memory = (1u << VMAFX_MEMORY_DEVICE_POINTER) | (1u << VMAFX_MEMORY_DMABUF) |
                            (1u << VMAFX_MEMORY_DEVICE_ARRAY) | (1u << VMAFX_MEMORY_GL_TEXTURE) |
                            (1u << VMAFX_MEMORY_VULKAN);
    mu_assert("fields", info.backend == VMAFX_BACKEND_HIP && info.index == 0 && info.flags == 0u &&
                            info.total_memory > 0u && info.name && info.name[0] != '\0' &&
                            info.fence_kinds == fences && info.memory_kinds == memory &&
                            info.pci[1] != UINT32_MAX);
    VmafxError *error = NULL;
    mu_assert("past the last",
              vmafx_device_info(VMAFX_BACKEND_HIP, (int32_t)count, &info, &error) ==
                      VMAFX_E_NOTFOUND &&
                  vt_failed(&error, VMAFX_E_NOTFOUND, "index", VMAFX_SUBJECT_DEVICE));
    VmafxDeviceInfo described = VMAFX_DEVICE_INFO_INIT;
    mu_assert("describe", vmafx_device_describe(gpu.device, &described, NULL) == VMAFX_OK &&
                              described.backend == VMAFX_BACKEND_HIP && described.index == 0 &&
                              !strcmp(described.name, info.name));
    return NULL;
}

static char *test_device_create_refusals(void)
{
    VmafxError *error = NULL;
    VmafxDevice *device = NULL;
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.backend = VMAFX_BACKEND_HIP;
    desc.flags = VMAFX_DEVICE_PROFILING;
    mu_assert("profiling",
              vmafx_device_create(&desc, &device, &error) == VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "desc.flags", VMAFX_SUBJECT_PARAMETER));
    desc.flags = 0u;
    desc.external[1] = 1u;
    mu_assert("a second handle",
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

/* A HIP device imports frames; it has no frame pools (ADR-2092). */
static char *test_no_frame_pools(void)
{
    VmafxFrameDesc desc = VMAFX_FRAME_DESC_INIT;
    desc.pix_fmt = VMAFX_PIXEL_FORMAT_YUV420P;
    desc.bpc = 8u;
    desc.w = 64u;
    desc.h = 64u;
    VmafxFramePool *pool = NULL;
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_frame_pool_create(gpu.device, &desc, 2u, &pool, &error);
    /* The refusal says what a HIP device does instead, not "host frames". */
    const bool says_import =
        error != NULL && strstr(vmafx_error_message(error), "vmafx_frame_import()") != NULL;
    mu_assert("refused, named",
              status == VMAFX_E_NOTSUP && says_import &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "device", VMAFX_SUBJECT_DEVICE) &&
                  pool == NULL);
    return NULL;
}

/* A device from the caller's stream: the library reads its frames on it. */
static char *test_device_external(void)
{
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.backend = VMAFX_BACKEND_HIP;
    desc.external[0] = vmaf_hip_stream_bits(gpu.producer);
    VmafxDevice *device = NULL;
    mu_assert("create", vmafx_device_create(&desc, &device, NULL) == VMAFX_OK);
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    mu_assert("describe", vmafx_device_describe(device, &info, NULL) == VMAFX_OK &&
                              info.backend == VMAFX_BACKEND_HIP && info.index == -1);
    VmafxContext *const context = vc_cell_context(device, &vc_cells[17]); /* psnr */
    mu_assert("context on it", context != NULL);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    vmafx_device_unref(device);
    /* The caller's stream survives the device. */
    mu_assert("stream kept", hipStreamQuery(gpu.producer) == hipSuccess);
    return NULL;
}

/* The context picks HIP twins once the device is attached. */
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
                          r.backend == VMAFX_BACKEND_HIP && strstr(r.extractor, "hip"));
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

static void m_host(VmafxFrameImport *imp)
{
    imp->memory = VMAFX_MEMORY_HOST;
}

static void m_modifier(VmafxFrameImport *imp)
{
    imp->plane[1].modifier = 0x100000000000002ull;
}

static void m_planar_array(VmafxFrameImport *imp)
{
    imp->memory = VMAFX_MEMORY_DEVICE_ARRAY;
}

static void m_gl_sync_on_pointers(VmafxFrameImport *imp)
{
    imp->acquire.kind = VMAFX_FENCE_GL_SYNC;
    imp->acquire.handle = 1u;
}

static void m_event_without_handle(VmafxFrameImport *imp)
{
    imp->acquire.kind = VMAFX_FENCE_HIP_EVENT;
}

static void m_other_backend_event(VmafxFrameImport *imp)
{
    imp->acquire.kind = VT_UNBUILT_FENCE;
    imp->acquire.handle = 1u;
}

static void m_sync_file_not_open(VmafxFrameImport *imp)
{
    imp->acquire.kind = VMAFX_FENCE_SYNC_FILE;
    imp->acquire.fd = 1000;
}

static void m_null_plane(VmafxFrameImport *imp)
{
    imp->plane[2].handle = 0u;
    imp->plane[2].offset = 0u;
}

static void m_short_pitch(VmafxFrameImport *imp)
{
    imp->plane[0].pitch = W - 1u;
}

static const Refusal refusals[] = {
    {"host memory", m_host, "desc.memory", VMAFX_E_NOTSUP, VMAFX_SUBJECT_PARAMETER},
    {"modifier", m_modifier, "desc.plane[1].modifier", VMAFX_E_NOTSUP, VMAFX_SUBJECT_PLANE},
    {"planar arrays copy", m_planar_array, "desc.memory", VMAFX_E_NOTSUP, VMAFX_SUBJECT_PARAMETER},
    {"GL sync on pointers", m_gl_sync_on_pointers, "desc.acquire.kind", VMAFX_E_INVALID,
     VMAFX_SUBJECT_FENCE},
    {"event handle", m_event_without_handle, "desc.acquire.handle", VMAFX_E_INVALID,
     VMAFX_SUBJECT_FENCE},
    {"another backend's fence", m_other_backend_event, "desc.acquire.kind", VMAFX_E_NOTSUP,
     VMAFX_SUBJECT_FENCE},
    {"sync_file not open", m_sync_file_not_open, "desc.acquire.fd", VMAFX_E_INVALID,
     VMAFX_SUBJECT_FENCE},
    {"plane address", m_null_plane, "desc.plane[2].handle", VMAFX_E_INVALID, VMAFX_SUBJECT_PLANE},
    {"pitch", m_short_pitch, "desc.plane[0].pitch", VMAFX_E_INVALID, VMAFX_SUBJECT_PLANE},
};

static char *refuse(const VmafxFrameImport *base, const Refusal *r)
{
    VmafxFrameImport imp = *base;
    r->mutate(&imp);
    VmafxError *error = NULL;
    VmafxFrame *frame = NULL;
    const VmafxStatus status = vmafx_frame_import(gpu.device, &imp, &frame, &error);
    const char *const subject = error ? vmafx_error_subject(error) : "";
    const bool named = status == r->status && vt_failed(&error, r->status, r->subject, r->kind);
    if (!named) {
        (void)fprintf(stderr, "\n  refusal %s: status %d, subject %s\n", r->what, (int)status,
                      subject);
    }
    vmafx_frame_unref(frame);
    mu_assert("refused, the field named", named && frame == NULL);
    return NULL;
}

static char *test_import_refusals(void)
{
    const Clip c = clip8;
    VhPlanes p;
    memset(&p, 0, sizeof(p));
    const bool up = vh_upload(&gpu, &c.desc, c.ref, VMAFX_PIXEL_FORMAT_NV12, 0u, 64u, &p);
    const VmafxFrameImport nv12 = vh_import_desc(&c.desc, VMAFX_PIXEL_FORMAT_NV12, 8u, &p);
    VhPlanes planar;
    memset(&planar, 0, sizeof(planar));
    const bool up2 = vh_upload(&gpu, &c.desc, c.ref, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 64u, &planar);
    const VmafxFrameImport yuv = vh_import_desc(&c.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &planar);
    char *msg = up && up2 ? NULL : "upload";
    for (size_t i = 0; i < sizeof(refusals) / sizeof(refusals[0]) && !msg; i++) {
        const bool planar_case =
            refusals[i].mutate == m_planar_array || refusals[i].mutate == m_null_plane;
        msg = refuse(planar_case ? &yuv : &nv12, &refusals[i]);
    }
    vh_free(&gpu, &p);
    vh_free(&gpu, &planar);
    return msg;
}

/* An unsignalled HOST acquire fence: the HIP device answers VMAFX_E_BUSY
 * (vmafx_context_import_frame() waits on the host and retries). */
static char *test_host_acquire_busy(void)
{
    const Clip c = clip8;
    VhPlanes p;
    memset(&p, 0, sizeof(p));
    mu_assert("upload", vh_upload(&gpu, &c.desc, c.ref, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u, &p));
    VmafxFrameImport imp = vh_import_desc(&c.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &p);
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
    vh_free(&gpu, &p);
    mu_assert("busy", busy == VMAFX_E_BUSY && named);
    mu_assert("imported once signalled", imported == VMAFX_OK);
    return NULL;
}

/* Release fences a HIP frame signals: HOST and HIP_EVENT; SYNC_FILE is
 * refused naming the kind (no HIP operation signals a kernel fence). */
static char *test_release_fence_kinds(void)
{
    const Clip c = clip8;
    VhPlanes p;
    memset(&p, 0, sizeof(p));
    mu_assert("upload", vh_upload(&gpu, &c.desc, c.ref, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u, &p));
    const VmafxFrameImport imp = vh_import_desc(&c.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &p);
    VmafxFrame *frame = NULL;
    mu_assert("import", vmafx_frame_import(gpu.device, &imp, &frame, NULL) == VMAFX_OK);
    VmafxFence fence = VMAFX_FENCE_INIT;
    VmafxError *error = NULL;
    const bool sync_file =
        vmafx_frame_release_fence(frame, VMAFX_FENCE_SYNC_FILE, &fence, &error) == VMAFX_E_NOTSUP &&
        vt_failed(&error, VMAFX_E_NOTSUP, "kind", VMAFX_SUBJECT_FENCE);
    const bool cuda = vmafx_frame_release_fence(frame, VMAFX_FENCE_CUDA_EVENT, &fence, &error) ==
                          VMAFX_E_NOTSUP &&
                      vt_failed(&error, VMAFX_E_NOTSUP, "kind", VMAFX_SUBJECT_FENCE);
    VmafxFence event = VMAFX_FENCE_INIT;
    const bool hip =
        vmafx_frame_release_fence(frame, VMAFX_FENCE_HIP_EVENT, &event, NULL) == VMAFX_OK &&
        event.kind == VMAFX_FENCE_HIP_EVENT && event.handle != 0u;
    /* Handed out before the frame is released: pending, not signalled. */
    const bool pending = vmafx_fence_wait(&event, 0u, NULL) == VMAFX_PENDING;
    vmafx_frame_unref(frame);
    const bool released = vmafx_fence_wait(&event, 5000000000ull, NULL) == VMAFX_OK;
    (void)vmafx_fence_destroy(&event, NULL);
    vh_free(&gpu, &p);
    mu_assert("SYNC_FILE refused, named", sync_file);
    mu_assert("another backend's kind refused", cuda);
    mu_assert("HIP_EVENT handed out", hip);
    mu_assert("pending until the release is recorded", pending);
    mu_assert("signalled after the release", released);
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

/* The host session of `c` on a context with `model` on the HIP device. */
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
        mu_assert("submit",
                  vmafx_submit(context, vmafx_frame_ref(frames[(size_t)2u * i]),
                               vmafx_frame_ref(frames[(size_t)2u * i + 1u]), i, NULL) == VMAFX_OK);
    }
    mu_assert("flush", vmafx_flush(context, NULL) == VMAFX_OK);
    return NULL;
}

/* Attempts at a comparison whose values differ: the gfx1036 this lane is
 * verified on now and then never runs a run of a stream's commands (a lost
 * accumulator clear, host frames included,
 * T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01); every attempt that differs
 * is reported, and a defect of the import differs in every attempt. */
#define ATTEMPTS 4u

/* One attempt: the import session and the host session, compared. */
static char *score_and_compare_once(const Clip *c, VmafxModel *model, VmafxFrame *const *frames,
                                    unsigned long *differing)
{
    VmafxContext *const imp = model_context(model);
    char *msg = imp ? submit_frames(imp, frames) : "context";
    VmafxContext *const host = msg ? NULL : run_host(c, model);
    unsigned long compared = 0;
    msg = msg ? msg :
                (host && vc_compare(host, imp, FRAMES, &compared, differing) ? NULL : "compare");
    (void)fprintf(stderr, "[%lu values, %lu differing] ", compared, *differing);
    if (imp) {
        (void)vmafx_context_destroy(imp, NULL);
    }
    if (host) {
        (void)vmafx_context_destroy(host, NULL);
    }
    return msg;
}

/* A context with `model` scoring `frames`, compared with the host session:
 * bit-identical in one of ATTEMPTS attempts. */
static char *score_and_compare(const Clip *c, VmafxModel *model, VmafxFrame *const *frames)
{
    unsigned long differing = 1;
    char *msg = NULL;
    for (unsigned a = 0; a < ATTEMPTS && differing != 0u && !msg; a++) {
        differing = 0;
        msg = score_and_compare_once(c, model, frames, &differing);
    }
    return msg ? msg : (differing == 0u ? NULL : "bit-identical");
}

/* ---- Any layout, bound ------------------------------------------------------------------ */

/* Planes at odd addresses with odd pitches are bound where they are (the HIP
 * twins read them through device copies): no flag, no copy, no conversion,
 * the scores of the host frames. */
static char *test_any_layout_bound(void)
{
    const Clip c = clip8;
    VmafxModel *model = NULL;
    VhPlanes planes[2u * FRAMES];
    VmafxFrame *frames[2u * FRAMES];
    memset(planes, 0, sizeof(planes));
    memset((void *)frames, 0, sizeof(frames));
    mu_assert("model", vmafx_model_load(NULL, MODEL, &model, NULL) == VMAFX_OK);
    const size_t frame = vt_frame_bytes(&c.desc);
    const uint64_t conversions = vmafx_test_conversions();
    bool ok = true;
    for (unsigned k = 0; k < 2u * FRAMES && ok; k++) {
        const uint8_t *const src = (k & 1u ? c.dist : c.ref) + (k / 2u) * frame;
        ok = vh_upload_skewed(&gpu, &c.desc, src, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 5u, 3u,
                              &planes[k]);
        const VmafxFrameImport imp =
            vh_import_desc(&c.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &planes[k]);
        ok = ok && vmafx_frame_import(gpu.device, &imp, &frames[k], NULL) == VMAFX_OK;
    }
    char *const msg = ok ? score_and_compare(&c, model, frames) : "import";
    for (unsigned k = 0; k < 2u * FRAMES; k++) {
        vmafx_frame_unref(frames[k]);
        vh_free(&gpu, &planes[k]);
    }
    vmafx_model_unref(model);
    mu_assert_msg(msg);
    mu_assert("bound: no conversion", vmafx_test_conversions() == conversions);
    mu_assert("no host copy", vmafx_test_host_copies() == 0u);
    return NULL;
}

/* ---- HIP arrays ------------------------------------------------------------------------ */

typedef struct Arrays {
    hipArray_t a[3];
    uint32_t n;
} Arrays;

static bool array_plane(const uint8_t *src, size_t row, unsigned w, unsigned h, unsigned ch,
                        int bits, hipArray_t *out)
{
    const hipChannelFormatDesc desc =
        hipCreateChannelDesc(bits, ch > 1u ? bits : 0, 0, 0, hipChannelFormatKindUnsigned);
    return hipMallocArray(out, &desc, w, h, hipArrayDefault) == hipSuccess &&
           hipMemcpy2DToArray(*out, 0, 0, src, row, row, h, hipMemcpyHostToDevice) == hipSuccess;
}

/* The frame in arrays as the producer holds it: planar, NV12, or P010
 * (16-bit channels, samples in the high bits). */
static bool arrays_make(const Clip *c, const uint8_t *planar, uint32_t pix_fmt, Arrays *a)
{
    unsigned w[3];
    unsigned h[3];
    size_t row[3];
    vt_plane_geometry(&c->desc, w, h, row);
    const bool semi = pix_fmt != VMAFX_PIXEL_FORMAT_YUV420P;
    const unsigned shift = pix_fmt == VMAFX_PIXEL_FORMAT_P010 ? 6u : 0u;
    const int bits = c->desc.bpc > 8u ? 16 : 8;
    uint8_t *staged = NULL;
    const uint8_t *src = vt_producer_bytes(&c->desc, planar, pix_fmt, shift, &staged);
    a->n = semi ? 2u : 3u;
    bool ok = src != NULL;
    for (uint32_t i = 0; i < a->n && i < 3u && ok; i++) {
        const unsigned ch = semi && i == 1u ? 2u : 1u;
        ok = array_plane(src, row[i] * ch, w[i], h[i], ch, bits, &a->a[i]);
        src += row[i] * ch * h[i];
    }
    free(staged);
    /* The producer's writes are complete before the import (the descriptor
     * carries no acquire fence): hipMemcpy2DToArray() may return before its
     * copy has landed. */
    return ok && hipDeviceSynchronize() == hipSuccess;
}

static void arrays_free(Arrays *a)
{
    (void)hipDeviceSynchronize();
    for (uint32_t i = 0; i < a->n; i++) {
        if (a->a[i]) {
            (void)hipFreeArray(a->a[i]);
        }
    }
    memset(a, 0, sizeof(*a));
}

static VmafxFrame *import_arrays(const Clip *c, const Arrays *a, uint32_t pix_fmt)
{
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    imp.memory = VMAFX_MEMORY_DEVICE_ARRAY;
    imp.pix_fmt = pix_fmt;
    imp.bpc = c->desc.bpc;
    imp.w = c->desc.w;
    imp.h = c->desc.h;
    imp.n_planes = a->n;
    imp.flags = pix_fmt == VMAFX_PIXEL_FORMAT_YUV420P ? VMAFX_IMPORT_ALLOW_COPY : 0u;
    for (uint32_t i = 0; i < a->n; i++) {
        imp.plane[i].handle = (uintptr_t)a->a[i];
    }
    VmafxFrame *frame = NULL;
    return vmafx_frame_import(gpu.device, &imp, &frame, NULL) == VMAFX_OK ? frame : NULL;
}

/* Every frame of `c` imported out of HIP arrays scores as the host frames. */
static char *arrays_case(const Clip *c, VmafxModel *model, uint32_t pix_fmt)
{
    Arrays arrays[2u * FRAMES];
    VmafxFrame *frames[2u * FRAMES];
    memset(arrays, 0, sizeof(arrays));
    memset((void *)frames, 0, sizeof(frames));
    const size_t frame = vt_frame_bytes(&c->desc);
    bool ok = true;
    for (unsigned k = 0; k < 2u * FRAMES && ok; k++) {
        const uint8_t *const src = (k & 1u ? c->dist : c->ref) + (k / 2u) * frame;
        ok = arrays_make(c, src, pix_fmt, &arrays[k]);
        frames[k] = ok ? import_arrays(c, &arrays[k], pix_fmt) : NULL;
        ok = ok && frames[k];
    }
    char *const msg = ok ? score_and_compare(c, model, frames) : "array import";
    for (unsigned k = 0; k < 2u * FRAMES; k++) {
        vmafx_frame_unref(frames[k]);
        arrays_free(&arrays[k]);
    }
    return msg;
}

/* NV12 and P010 arrays are converted, planar arrays copied (with the flag). */
static char *test_arrays(void)
{
    const Clip c = clip8;
    Clip c10;
    memset(&c10, 0, sizeof(c10));
    VmafxModel *model = NULL;
    mu_assert("model", vmafx_model_load(NULL, MODEL, &model, NULL) == VMAFX_OK);
    char *msg = arrays_case(&c, model, VMAFX_PIXEL_FORMAT_NV12);
    msg = msg ? msg : arrays_case(&c, model, VMAFX_PIXEL_FORMAT_YUV420P);
    msg = msg ? msg : (clip_make(&c10, 10u) ? NULL : "10-bit clip");
    msg = msg ? msg : arrays_case(&c10, model, VMAFX_PIXEL_FORMAT_P010);
    clip_free(&c10);
    vmafx_model_unref(model);
    return msg;
}

/* ---- dma-bufs ------------------------------------------------------------------------- */

#ifdef VMAFX_TEST_HAVE_GBM

static void m_dmabuf_tiled(VmafxFrameImport *imp)
{
    imp->plane[0].modifier = 0x200000000000001ull; /* an AMD tiled modifier */
}

static void m_dmabuf_oversize(VmafxFrameImport *imp)
{
    imp->plane[1].size *= 2u;
}

static void m_dmabuf_no_fd(VmafxFrameImport *imp)
{
    imp->plane[0].fd = -1;
}

static void m_dmabuf_plane_index(VmafxFrameImport *imp)
{
    imp->plane[1].plane_index = 1u;
}

static void m_dmabuf_rows_past_end(VmafxFrameImport *imp)
{
    imp->plane[1].offset = imp->plane[1].size - 64u;
}

static const Refusal dmabuf_refusals[] = {
    {"tiled", m_dmabuf_tiled, "desc.plane[0].modifier", VMAFX_E_NOTSUP, VMAFX_SUBJECT_PLANE},
    {"size above the buffer", m_dmabuf_oversize, "desc.plane[1].size", VMAFX_E_RANGE,
     VMAFX_SUBJECT_PLANE},
    {"no descriptor", m_dmabuf_no_fd, "desc.plane[0].fd", VMAFX_E_INVALID, VMAFX_SUBJECT_PLANE},
    {"plane index", m_dmabuf_plane_index, "desc.plane[1].plane_index", VMAFX_E_INVALID,
     VMAFX_SUBJECT_PLANE},
    {"rows past the end", m_dmabuf_rows_past_end, "desc.plane[1].size", VMAFX_E_RANGE,
     VMAFX_SUBJECT_PLANE},
};

/* Every frame of `c` written into dma-bufs as `pix_fmt` and imported with
 * the dma-buf's sync_file through the import rule. */
static char *dmabuf_case(const Clip *c, VmafxModel *model, uint32_t pix_fmt)
{
    VhDmabuf bufs[2u * FRAMES];
    VmafxFrame *frames[2u * FRAMES];
    memset(bufs, 0, sizeof(bufs));
    memset((void *)frames, 0, sizeof(frames));
    VmafxContext *importer = model_context(model);
    const size_t frame = vt_frame_bytes(&c->desc);
    bool ok = importer != NULL;
    for (unsigned k = 0; k < 2u * FRAMES && ok; k++) {
        const uint8_t *const src = (k & 1u ? c->dist : c->ref) + (k / 2u) * frame;
        ok = vh_dmabuf_write(&gbm, &c->desc, src, pix_fmt, 0u, 24u, &bufs[k]);
        VmafxFrameImport imp = vh_dmabuf_desc(&c->desc, pix_fmt, 8u, &bufs[k]);
        imp.acquire.kind = VMAFX_FENCE_SYNC_FILE;
        imp.acquire.fd = ok ? vh_dmabuf_sync_file(&bufs[k]) : -1;
        ok = ok && imp.acquire.fd >= 0 &&
             vmafx_context_import_frame(importer, gpu.device, &imp, "main", &frames[k], NULL) ==
                 VMAFX_OK;
        if (imp.acquire.fd >= 0) {
            (void)close(imp.acquire.fd);
        }
    }
    char *const msg = ok ? score_and_compare(c, model, frames) : "dma-buf import";
    for (unsigned k = 0; k < 2u * FRAMES; k++) {
        vmafx_frame_unref(frames[k]);
    }
    if (importer) {
        (void)vmafx_context_destroy(importer, NULL);
    }
    (void)hipDeviceSynchronize();
    for (unsigned k = 0; k < 2u * FRAMES; k++) {
        vh_dmabuf_free(&bufs[k]);
    }
    return msg;
}

static char *dmabuf_refusals_case(const Clip *c)
{
    VhDmabuf b;
    mu_assert("write",
              vh_dmabuf_write(&gbm, &c->desc, c->ref, VMAFX_PIXEL_FORMAT_NV12, 0u, 0u, &b));
    const VmafxFrameImport nv12 = vh_dmabuf_desc(&c->desc, VMAFX_PIXEL_FORMAT_NV12, 8u, &b);
    char *msg = NULL;
    for (size_t i = 0; i < sizeof(dmabuf_refusals) / sizeof(dmabuf_refusals[0]) && !msg; i++) {
        msg = refuse(&nv12, &dmabuf_refusals[i]);
    }
    vh_dmabuf_free(&b);
    return msg;
}

static char *test_dmabuf(void)
{
    if (!have_gbm) {
        (void)fprintf(stderr, "[no render node: dma-buf cases skipped] ");
        return NULL;
    }
    const Clip c = clip8;
    VmafxModel *model = NULL;
    mu_assert("model", vmafx_model_load(NULL, MODEL, &model, NULL) == VMAFX_OK);
    char *msg = dmabuf_refusals_case(&c);
    msg = msg ? msg : dmabuf_case(&c, model, VMAFX_PIXEL_FORMAT_YUV420P);
    msg = msg ? msg : dmabuf_case(&c, model, VMAFX_PIXEL_FORMAT_NV12);
    vmafx_model_unref(model);
    mu_assert_msg(msg);
    mu_assert("no host copy", vmafx_test_host_copies() == 0u);
    return NULL;
}

/* A sync_file the writer has not signalled yet: a raw import is
 * VMAFX_E_BUSY naming the fence; the import rule waits for it and retries. */
static char *test_sync_file_busy(void)
{
    if (!have_gbm) {
        return NULL;
    }
    const Clip c = clip8;
    unsigned busy = 0;
    unsigned imported = 0;
    for (unsigned round = 0; round < 8u; round++) {
        VhDmabuf b;
        mu_assert("write",
                  vh_dmabuf_write(&gbm, &c.desc, c.ref, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u, &b));
        VmafxFrameImport imp = vh_dmabuf_desc(&c.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &b);
        imp.acquire.kind = VMAFX_FENCE_SYNC_FILE;
        imp.acquire.fd = vh_dmabuf_sync_file(&b);
        VmafxError *error = NULL;
        VmafxFrame *frame = NULL;
        const VmafxStatus status = vmafx_frame_import(gpu.device, &imp, &frame, &error);
        if (status == VMAFX_E_BUSY) {
            busy += vt_failed(&error, VMAFX_E_BUSY, "desc.acquire", VMAFX_SUBJECT_FENCE);
            imported += vmafx_fence_wait(&imp.acquire, 5000000000ull, NULL) == VMAFX_OK &&
                        vmafx_frame_import(gpu.device, &imp, &frame, NULL) == VMAFX_OK;
        } else {
            imported += status == VMAFX_OK;
        }
        vmafx_error_free(error);
        vmafx_frame_unref(frame);
        (void)close(imp.acquire.fd);
        (void)hipDeviceSynchronize();
        vh_dmabuf_free(&b);
    }
    (void)fprintf(stderr, "[%u of 8 sync_files pending at import] ", busy);
    mu_assert("an unsignalled sync_file is BUSY, named", busy > 0u);
    mu_assert("imported once signalled", imported == 8u);
    return NULL;
}

#else

static char *test_dmabuf(void)
{
    (void)fprintf(stderr, "[built without libgbm: dma-buf cases skipped] ");
    return NULL;
}

static char *test_sync_file_busy(void)
{
    return NULL;
}

#endif /* VMAFX_TEST_HAVE_GBM */

/* ---- Admission ------------------------------------------------------------------------ */

/* An extractor without a HIP twin runs on the CPU: it would need a host copy
 * of the imported frame and is named. */
static char *test_admission(void)
{
    const Clip c = clip8;
    VhPlanes p;
    memset(&p, 0, sizeof(p));
    mu_assert("upload", vh_upload(&gpu, &c.desc, c.ref, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u, &p));
    VmafxContext *context = NULL;
    mu_assert("context",
              vmafx_context_create(NULL, &context, NULL) == VMAFX_OK &&
                  vmafx_context_use_device(context, gpu.device, NULL) == VMAFX_OK &&
                  vmafx_context_use_feature(context, "psnr", NULL, NULL) == VMAFX_OK &&
                  vmafx_context_use_feature(context, "delta_e_itp", NULL, NULL) == VMAFX_OK);
    const VmafxFrameImport imp = vh_import_desc(&c.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &p);
    VmafxFrame *frame = NULL;
    VmafxError *error = NULL;
    const VmafxStatus status =
        vmafx_context_import_frame(context, gpu.device, &imp, "main", &frame, &error);
    const char *const msg = vmafx_error_message(error);
    const bool named = status == VMAFX_E_NOTSUP && msg && strstr(msg, "delta_e_itp (cpu") &&
                       !strstr(msg, "psnr") && strstr(msg, "main: backend hip device 0");
    if (!named) {
        (void)fprintf(stderr, "\n  status %d: %s\n", (int)status, msg ? msg : "(none)");
    }
    vmafx_error_free(error);
    (void)vmafx_context_destroy(context, NULL);
    vh_free(&gpu, &p);
    mu_assert("the CPU extractor named, the HIP twin admitted", named && frame == NULL);
    return NULL;
}

/* ---- One import, two contexts ------------------------------------------------------------ */

typedef struct Shared {
    Clip clip;
    VmafxModel *models[2];
    VmafxContext *contexts[2];
    VhPlanes planes[2u * FRAMES];
    VmafxFence event; /* HIP_EVENT release fence of the last reference frame */
    VmafxFence host;  /* HOST release fence of the same frame */
} Shared;

static char *shared_submit(Shared *s)
{
    const size_t frame = vt_frame_bytes(&s->clip.desc);
    const uint64_t attempts = vmafx_test_import_attempts();
    for (unsigned i = 0; i < FRAMES; i++) {
        VmafxFrame *pair[2] = {NULL, NULL};
        for (unsigned side = 0; side < 2u; side++) {
            VhPlanes *const p = &s->planes[(size_t)2u * i + side];
            const uint8_t *const src = (side ? s->clip.dist : s->clip.ref) + i * frame;
            mu_assert("upload",
                      vh_upload(&gpu, &s->clip.desc, src, VMAFX_PIXEL_FORMAT_NV12, 0u, 16u, p));
            VmafxFrameImport imp = vh_import_desc(&s->clip.desc, VMAFX_PIXEL_FORMAT_NV12, 8u, p);
            hipEvent_t ev = NULL;
            imp.acquire = vh_record(&gpu, &ev);
            mu_assert("import",
                      vmafx_frame_import(gpu.device, &imp, &pair[side], NULL) == VMAFX_OK);
            (void)hipEventDestroy(ev);
        }
        (void)vmafx_fence_destroy(&s->event, NULL);
        (void)vmafx_fence_destroy(&s->host, NULL);
        mu_assert("fences", vmafx_frame_release_fence(pair[0], VMAFX_FENCE_HIP_EVENT, &s->event,
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
        VmafxContext *const alone = run_host(&s->clip, s->models[k]);
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
        vh_free(&gpu, &s->planes[k]);
    }
}

static char *one_import_two_contexts_once(void)
{
    Shared s;
    memset(&s, 0, sizeof(s));
    s.event = (VmafxFence)VMAFX_FENCE_INIT;
    s.host = (VmafxFence)VMAFX_FENCE_INIT;
    char *msg = (s.clip = clip8, true) &&
                        vmafx_model_load(NULL, MODEL, &s.models[0], NULL) == VMAFX_OK &&
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

/* One import scored by two contexts; a comparison that differs is run
 * again (ATTEMPTS), the release order is asserted on every attempt. */
static char *test_one_import_two_contexts(void)
{
    char *msg = "not run";
    for (unsigned a = 0; a < ATTEMPTS && msg; a++) {
        msg = one_import_two_contexts_once();
        if (msg && strcmp(msg, "each context equals its own run") != 0) {
            break;
        }
    }
    return msg;
}

/* ---- Release callback ------------------------------------------------------------------- */

/* What the release callback saw. */
typedef struct Callback {
    VmafxFence event;
    unsigned calls;
    bool recorded; /* the HIP_EVENT release fence was recorded when it ran */
    bool waited;   /* the producer's stream waits on it */
} Callback;

static void on_release(void *user)
{
    Callback *const cb = user;
    cb->calls++;
    /* Recorded: the wait ends once the device passes the event. Not yet
     * recorded (a callback run too early), it would be recorded only after
     * this returns, on this thread, so the wait runs out. One poll would race
     * with the device finishing the event. */
    cb->recorded = vmafx_fence_wait(&cb->event, 5000000000ull, NULL) == VMAFX_OK;
    cb->waited = hipStreamWaitEvent(gpu.producer, vh_event(&cb->event), 0u) == hipSuccess;
}

/* The callback runs once, on the releasing thread, after the HIP_EVENT
 * release fence is recorded: a producer makes its stream wait on it there. */
static char *test_release_callback(void)
{
    const Clip c = clip8;
    VhPlanes p;
    memset(&p, 0, sizeof(p));
    mu_assert("upload", vh_upload(&gpu, &c.desc, c.ref, VMAFX_PIXEL_FORMAT_NV12, 0u, 0u, &p));
    VmafxFrameImport imp = vh_import_desc(&c.desc, VMAFX_PIXEL_FORMAT_NV12, 8u, &p);
    Callback cb;
    memset(&cb, 0, sizeof(cb));
    cb.event = (VmafxFence)VMAFX_FENCE_INIT;
    imp.release = on_release;
    imp.user = &cb;
    VmafxFrame *frame = NULL;
    mu_assert("import", vmafx_frame_import(gpu.device, &imp, &frame, NULL) == VMAFX_OK);
    mu_assert("fence",
              vmafx_frame_release_fence(frame, VMAFX_FENCE_HIP_EVENT, &cb.event, NULL) == VMAFX_OK);
    vmafx_frame_unref(frame);
    const bool drained = hipStreamSynchronize(gpu.producer) == hipSuccess;
    (void)vmafx_fence_destroy(&cb.event, NULL);
    vh_free(&gpu, &p);
    mu_assert("called once", cb.calls == 1u);
    mu_assert("after the recording", cb.recorded);
    mu_assert("the producer's stream waits on it", cb.waited && drained);
    return NULL;
}

/* ---- Host copies ------------------------------------------------------------------------ */

/* The planted host copy (VMAFX_TEST_FORCE_HOST_COPY) is counted, one per
 * plane; without it the count stays 0. */
static char *test_host_copy_counter(void)
{
    mu_assert("no host copy so far", vmafx_test_host_copies() == 0u);
    const Clip c = clip8;
    VhPlanes p;
    memset(&p, 0, sizeof(p));
    mu_assert("upload", vh_upload(&gpu, &c.desc, c.ref, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 8u, &p));
    const VmafxFrameImport imp = vh_import_desc(&c.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &p);
    VmafxFrame *frame = NULL;
    vmafx_test_set_switches(VMAFX_TEST_FORCE_HOST_COPY);
    const VmafxStatus status = vmafx_frame_import(gpu.device, &imp, &frame, NULL);
    vmafx_test_set_switches(0u);
    vmafx_frame_unref(frame);
    const uint64_t copies = vmafx_test_host_copies();
    vmafx_test_reset_counters();
    vh_free(&gpu, &p);
    mu_assert("planted", status == VMAFX_OK && copies == 3u);
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
GUARDED(test_no_frame_pools)
GUARDED(test_device_external)
GUARDED(test_context_device)
GUARDED(test_import_refusals)
GUARDED(test_host_acquire_busy)
GUARDED(test_release_fence_kinds)
GUARDED(test_any_layout_bound)
GUARDED(test_arrays)
GUARDED(test_dmabuf)
GUARDED(test_sync_file_busy)
GUARDED(test_admission)
GUARDED(test_one_import_two_contexts)
GUARDED(test_release_callback)
GUARDED(test_host_copy_counter)

char *run_tests(void)
{
    vmafx_test_reset_counters();
    have_gpu = vh_open(&gpu) && clip_make(&clip8, 8u);
#ifdef VMAFX_TEST_HAVE_GBM
    have_gbm = have_gpu && vh_gbm_open(&gpu, &gbm);
#endif
    static const MuTest tests[] = {
        MU_TEST(test_device_info_g),
        MU_TEST(test_device_create_refusals_g),
        MU_TEST(test_no_frame_pools_g),
        MU_TEST(test_device_external_g),
        MU_TEST(test_context_device_g),
        MU_TEST(test_import_refusals_g),
        MU_TEST(test_host_acquire_busy_g),
        MU_TEST(test_release_fence_kinds_g),
        MU_TEST(test_any_layout_bound_g),
        MU_TEST(test_arrays_g),
        MU_TEST(test_dmabuf_g),
        MU_TEST(test_sync_file_busy_g),
        MU_TEST(test_admission_g),
        MU_TEST(test_one_import_two_contexts_g),
        MU_TEST(test_release_callback_g),
        MU_TEST(test_host_copy_counter_g),
    };
    char *const msg = mu_run_table(tests, MU_TABLE_LEN(tests));
#ifdef VMAFX_TEST_HAVE_GBM
    if (have_gbm) {
        vh_gbm_close(&gbm);
    }
#endif
    clip_free(&clip8);
    vh_close(&gpu);
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
