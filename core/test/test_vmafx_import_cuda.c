/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The VMAFx device-frame API on a CUDA device (RC4 WP3, ADR-1929, ADR-2023):
 * devices by index and from the caller's context and stream, the engine
 * import of a device into a context, every refusal of a CUDA import with its
 * named field, NV12 / planar imports out of CUDA arrays, CUDA frame pools,
 * per-extractor admission, one import scored by two contexts with its
 * release fences signalled after the last reader of either, and the
 * host-copy counter (0, and 1 per plane under the planted host copy).
 *
 * Needs a CUDA device (77 without one).
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
#include "vmafx_cuda_test_util.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define W 352u
#define H 288u
#define FRAMES 4u
#define MODEL "vmaf_v1.0.16_3d0h"

static VcGpu gpu;
static bool have_gpu;

/* Two frames per index of deterministic 8-bit 4:2:0 content. */
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
              vmafx_device_count(VMAFX_BACKEND_CUDA, &count, NULL) == VMAFX_OK && count >= 1u);
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    mu_assert("info", vmafx_device_info(VMAFX_BACKEND_CUDA, 0, &info, NULL) == VMAFX_OK);
    const uint32_t fences = (1u << VMAFX_FENCE_NONE) | (1u << VMAFX_FENCE_HOST) |
                            (1u << VMAFX_FENCE_CUDA_EVENT) | (1u << VMAFX_FENCE_GL_SYNC);
    mu_assert("fields", info.backend == VMAFX_BACKEND_CUDA && info.index == 0 && info.flags == 0u &&
                            info.total_memory > 0u && info.name && info.name[0] != '\0' &&
                            info.fence_kinds == fences &&
                            (info.memory_kinds & (1u << VMAFX_MEMORY_DEVICE_POINTER)) &&
                            (info.memory_kinds & (1u << VMAFX_MEMORY_GL_TEXTURE)) &&
                            !(info.memory_kinds & (1u << VMAFX_MEMORY_HOST)));
    VmafxError *error = NULL;
    mu_assert("past the last",
              vmafx_device_info(VMAFX_BACKEND_CUDA, (int32_t)count, &info, &error) ==
                      VMAFX_E_NOTFOUND &&
                  vt_failed(&error, VMAFX_E_NOTFOUND, "index", VMAFX_SUBJECT_DEVICE));
    VmafxDeviceInfo described = VMAFX_DEVICE_INFO_INIT;
    mu_assert("describe", vmafx_device_describe(gpu.device, &described, NULL) == VMAFX_OK &&
                              described.backend == VMAFX_BACKEND_CUDA && described.index == 0 &&
                              !strcmp(described.name, info.name));
    return NULL;
}

static char *test_device_create_refusals(void)
{
    VmafxError *error = NULL;
    VmafxDevice *device = NULL;
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.backend = VMAFX_BACKEND_CUDA;
    desc.flags = VMAFX_DEVICE_PROFILING;
    mu_assert("profiling",
              vmafx_device_create(&desc, &device, &error) == VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "desc.flags", VMAFX_SUBJECT_PARAMETER));
    desc.flags = 0u;
    desc.external[1] = 1u;
    mu_assert("stream alone",
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

/* A device from the caller's context and stream: the library uses them. */
static char *test_device_external(void)
{
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.backend = VMAFX_BACKEND_CUDA;
    desc.external[0] = (uintptr_t)gpu.ctx;
    desc.external[1] = (uintptr_t)gpu.producer;
    VmafxDevice *device = NULL;
    mu_assert("create", vmafx_device_create(&desc, &device, NULL) == VMAFX_OK);
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    mu_assert("describe", vmafx_device_describe(device, &info, NULL) == VMAFX_OK &&
                              info.backend == VMAFX_BACKEND_CUDA && info.index == -1);
    VmafxContext *const context = vc_cell_context(device, &vc_cells[17]); /* psnr */
    mu_assert("context on it", context != NULL);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    vmafx_device_unref(device);
    /* The caller's stream survives the device. */
    mu_assert("stream kept", vc_push(&gpu) && gpu.f->cuStreamQuery(gpu.producer) == CUDA_SUCCESS);
    vc_pop(&gpu);
    return NULL;
}

/* The context picks CUDA twins once the device is attached. */
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
                          r.backend == VMAFX_BACKEND_CUDA && strstr(r.extractor, "cuda"));
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

static void m_dmabuf(VmafxFrameImport *imp)
{
    imp->memory = VMAFX_MEMORY_DMABUF;
}

static void m_modifier(VmafxFrameImport *imp)
{
    imp->plane[1].modifier = 0x100000000000002ull;
}

static void m_planar_array(VmafxFrameImport *imp)
{
    imp->memory = VMAFX_MEMORY_DEVICE_ARRAY;
}

static void m_sync_file(VmafxFrameImport *imp)
{
    imp->acquire.kind = VMAFX_FENCE_SYNC_FILE;
    imp->acquire.fd = 3;
}

static void m_gl_sync_on_pointers(VmafxFrameImport *imp)
{
    imp->acquire.kind = VMAFX_FENCE_GL_SYNC;
    imp->acquire.handle = 1u;
}

static void m_event_without_handle(VmafxFrameImport *imp)
{
    imp->acquire.kind = VMAFX_FENCE_CUDA_EVENT;
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
    {"dma-buf", m_dmabuf, "desc.memory", VMAFX_E_NOTSUP, VMAFX_SUBJECT_PARAMETER},
    {"modifier", m_modifier, "desc.plane[1].modifier", VMAFX_E_NOTSUP, VMAFX_SUBJECT_PLANE},
    {"planar arrays copy", m_planar_array, "desc.memory", VMAFX_E_NOTSUP, VMAFX_SUBJECT_PARAMETER},
    {"sync_file", m_sync_file, "desc.acquire.kind", VMAFX_E_NOTSUP, VMAFX_SUBJECT_FENCE},
    {"GL sync on pointers", m_gl_sync_on_pointers, "desc.acquire.kind", VMAFX_E_INVALID,
     VMAFX_SUBJECT_FENCE},
    {"event handle", m_event_without_handle, "desc.acquire.handle", VMAFX_E_INVALID,
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
    const bool named = status == r->status && vt_failed(&error, r->status, r->subject, r->kind);
    if (!named) {
        (void)fprintf(stderr, "\n  refusal %s: status %d\n", r->what, (int)status);
    }
    vmafx_frame_unref(frame);
    mu_assert("refused, the field named", named && frame == NULL);
    return NULL;
}

/* A bound plane the CUDA twins cannot read where it is (an unaligned
 * start, or an unrounded pitch) is refused naming the field, never copied
 * without VMAFX_IMPORT_ALLOW_COPY. */
static char *refuse_unaligned(const Clip *c)
{
    VcPlanes p;
    memset(&p, 0, sizeof(p));
    mu_assert("upload",
              vc_upload_skewed(&gpu, &c->desc, c->ref, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 5u, 3u, &p));
    VmafxFrameImport imp = vc_import_desc(&c->desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &p);
    VmafxError *error = NULL;
    VmafxFrame *frame = NULL;
    const bool start =
        vmafx_frame_import(gpu.device, &imp, &frame, &error) == VMAFX_E_NOTSUP &&
        vt_failed(&error, VMAFX_E_NOTSUP, "desc.plane[0].offset", VMAFX_SUBJECT_PLANE);
    imp.plane[0].offset -= 3u; /* aligned start, the pitch still unrounded */
    const bool pitch =
        vmafx_frame_import(gpu.device, &imp, &frame, &error) == VMAFX_E_NOTSUP &&
        vt_failed(&error, VMAFX_E_NOTSUP, "desc.plane[0].pitch", VMAFX_SUBJECT_PLANE);
    vc_free(&gpu, &p);
    mu_assert("unaligned start named", start && frame == NULL);
    mu_assert("unrounded pitch named", pitch && frame == NULL);
    return NULL;
}

static char *test_import_refusals(void)
{
    const Clip c = clip8;
    VcPlanes p;
    memset(&p, 0, sizeof(p));
    const bool up = vc_upload(&gpu, &c.desc, c.ref, VMAFX_PIXEL_FORMAT_NV12, 0u, 64u, &p);
    const VmafxFrameImport nv12 = vc_import_desc(&c.desc, VMAFX_PIXEL_FORMAT_NV12, 8u, &p);
    VcPlanes planar;
    memset(&planar, 0, sizeof(planar));
    const bool up2 = vc_upload(&gpu, &c.desc, c.ref, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 64u, &planar);
    const VmafxFrameImport yuv = vc_import_desc(&c.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &planar);
    char *msg = up && up2 ? NULL : "upload";
    for (size_t i = 0; i < sizeof(refusals) / sizeof(refusals[0]) && !msg; i++) {
        const bool planar_case =
            refusals[i].mutate == m_planar_array || refusals[i].mutate == m_null_plane;
        msg = refuse(planar_case ? &yuv : &nv12, &refusals[i]);
    }
    msg = msg ? msg : refuse_unaligned(&c);
    vc_free(&gpu, &p);
    vc_free(&gpu, &planar);
    return msg;
}

/* An unsignalled HOST acquire fence: the CUDA device answers VMAFX_E_BUSY
 * (vmafx_context_import_frame() waits on the host and retries). */
static char *test_host_acquire_busy(void)
{
    const Clip c = clip8;
    VcPlanes p;
    memset(&p, 0, sizeof(p));
    mu_assert("upload", vc_upload(&gpu, &c.desc, c.ref, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u, &p));
    VmafxFrameImport imp = vc_import_desc(&c.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &p);
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
    vc_free(&gpu, &p);
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

/* The host session of `c` on a context with `model` on the CUDA device. */
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

/* ---- CUDA arrays ---------------------------------------------------------------------- */

/* `c`'s frame `i` side `s` in CUDA arrays: NV12 as an 8-bit and a two-channel
 * array, or planar as three. */
typedef struct Arrays {
    CUarray a[3];
    uint32_t n;
} Arrays;

static bool array_plane(const uint8_t *src, size_t row, unsigned w, unsigned h, unsigned ch,
                        CUarray *out)
{
    CUDA_ARRAY3D_DESCRIPTOR d;
    memset(&d, 0, sizeof(d));
    d.Width = w;
    d.Height = h;
    d.Format = CU_AD_FORMAT_UNSIGNED_INT8;
    d.NumChannels = ch;
    CUDA_MEMCPY2D m = {.srcMemoryType = CU_MEMORYTYPE_HOST,
                       .srcHost = src,
                       .srcPitch = row,
                       .dstMemoryType = CU_MEMORYTYPE_ARRAY,
                       .WidthInBytes = row,
                       .Height = h};
    if (gpu.f->cuArray3DCreate(out, &d) != CUDA_SUCCESS) {
        return false;
    }
    m.dstArray = *out;
    return gpu.f->cuMemcpy2D(&m) == CUDA_SUCCESS;
}

static bool arrays_make(const Clip *c, const uint8_t *planar, bool nv12, Arrays *a)
{
    unsigned w[3];
    unsigned h[3];
    size_t row[3];
    vt_plane_geometry(&c->desc, w, h, row);
    uint8_t *const semi = nv12 ? malloc(vt_frame_bytes(&c->desc)) : NULL;
    const uint8_t *src = planar;
    if (semi) {
        vt_to_semiplanar(&c->desc, planar, 0u, semi);
        src = semi;
    }
    a->n = nv12 ? 2u : 3u;
    bool ok = vc_push(&gpu) && (!nv12 || semi);
    for (uint32_t i = 0; i < a->n && i < 3u && ok; i++) {
        const unsigned ch = nv12 && i == 1u ? 2u : 1u;
        ok = array_plane(src, row[i] * ch, w[i], h[i], ch, &a->a[i]);
        src += row[i] * ch * h[i];
    }
    vc_pop(&gpu);
    free(semi);
    return ok;
}

static void arrays_free(Arrays *a)
{
    if (vc_push(&gpu)) {
        (void)gpu.f->cuCtxSynchronize();
        for (uint32_t i = 0; i < a->n; i++) {
            if (a->a[i]) {
                (void)gpu.f->cuArrayDestroy(a->a[i]);
            }
        }
        vc_pop(&gpu);
    }
    memset(a, 0, sizeof(*a));
}

static VmafxFrame *import_arrays(const Clip *c, const Arrays *a, bool nv12)
{
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    imp.memory = VMAFX_MEMORY_DEVICE_ARRAY;
    imp.pix_fmt = nv12 ? VMAFX_PIXEL_FORMAT_NV12 : VMAFX_PIXEL_FORMAT_YUV420P;
    imp.bpc = 8u;
    imp.w = c->desc.w;
    imp.h = c->desc.h;
    imp.n_planes = a->n;
    imp.flags = nv12 ? 0u : VMAFX_IMPORT_ALLOW_COPY;
    for (uint32_t i = 0; i < a->n; i++) {
        imp.plane[i].handle = (uintptr_t)a->a[i];
    }
    VmafxFrame *frame = NULL;
    return vmafx_frame_import(gpu.device, &imp, &frame, NULL) == VMAFX_OK ? frame : NULL;
}

/* Every frame of `c` imported out of CUDA arrays scores as the host frames. */
static char *arrays_case(const Clip *c, VmafxModel *model, bool nv12)
{
    Arrays arrays[2u * FRAMES];
    VmafxFrame *frames[2u * FRAMES];
    memset(arrays, 0, sizeof(arrays));
    memset((void *)frames, 0, sizeof(frames));
    const size_t frame = vt_frame_bytes(&c->desc);
    bool ok = true;
    for (unsigned k = 0; k < 2u * FRAMES && ok; k++) {
        const uint8_t *const src = (k & 1u ? c->dist : c->ref) + (k / 2u) * frame;
        ok = arrays_make(c, src, nv12, &arrays[k]);
        frames[k] = ok ? import_arrays(c, &arrays[k], nv12) : NULL;
        ok = ok && frames[k];
    }
    char *const msg = ok ? score_and_compare(c, model, frames) : "array import";
    for (unsigned k = 0; k < 2u * FRAMES; k++) {
        vmafx_frame_unref(frames[k]);
        arrays_free(&arrays[k]);
    }
    return msg;
}

static char *test_arrays(void)
{
    const Clip c = clip8;
    VmafxModel *model = NULL;
    mu_assert("model", vmafx_model_load(NULL, MODEL, &model, NULL) == VMAFX_OK);
    char *msg = arrays_case(&c, model, true);
    msg = msg ? msg : arrays_case(&c, model, false);
    vmafx_model_unref(model);
    return msg;
}

/* ---- Unaligned planes with VMAFX_IMPORT_ALLOW_COPY --------------------------------------- */

/* Planes at unaligned addresses with unrounded pitches, copied on the device
 * with the flag: the frames score as the host frames, and no host copy. */
static char *test_unaligned_allow_copy(void)
{
    const Clip c = clip8;
    VmafxModel *model = NULL;
    VcPlanes planes[2u * FRAMES];
    VmafxFrame *frames[2u * FRAMES];
    memset(planes, 0, sizeof(planes));
    memset((void *)frames, 0, sizeof(frames));
    mu_assert("model", vmafx_model_load(NULL, MODEL, &model, NULL) == VMAFX_OK);
    const size_t frame = vt_frame_bytes(&c.desc);
    bool ok = true;
    for (unsigned k = 0; k < 2u * FRAMES && ok; k++) {
        const uint8_t *const src = (k & 1u ? c.dist : c.ref) + (k / 2u) * frame;
        ok = vc_upload_skewed(&gpu, &c.desc, src, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 5u, 3u,
                              &planes[k]);
        VmafxFrameImport imp = vc_import_desc(&c.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &planes[k]);
        imp.flags = VMAFX_IMPORT_ALLOW_COPY;
        CUevent ev = NULL;
        imp.acquire = vc_record(&gpu, &ev);
        ok = ok && vmafx_frame_import(gpu.device, &imp, &frames[k], NULL) == VMAFX_OK;
        (void)gpu.f->cuEventDestroy(ev);
    }
    char *const msg = ok ? score_and_compare(&c, model, frames) : "import";
    for (unsigned k = 0; k < 2u * FRAMES; k++) {
        vmafx_frame_unref(frames[k]);
        vc_free(&gpu, &planes[k]);
    }
    vmafx_model_unref(model);
    mu_assert_msg(msg);
    mu_assert("no host copy", vmafx_test_host_copies() == 0u);
    return NULL;
}

/* ---- Pools ---------------------------------------------------------------------------- */

/* Write the planar frame `src` into a CUDA pool frame's device planes. */
static bool fill_pool_frame(const Clip *c, VmafxFrame *frame, const uint8_t *src)
{
    VmafxFramePlanes p = VMAFX_FRAME_PLANES_INIT;
    unsigned w[3];
    unsigned h[3];
    size_t row[3];
    vt_plane_geometry(&c->desc, w, h, row);
    bool ok = vmafx_frame_planes(frame, &p, NULL) == VMAFX_OK && vc_push(&gpu);
    for (uint32_t i = 0; i < p.n_planes && i < 3u && ok; i++) {
        CUDA_MEMCPY2D m = {.srcMemoryType = CU_MEMORYTYPE_HOST,
                           .srcHost = src,
                           .srcPitch = row[i],
                           .dstMemoryType = CU_MEMORYTYPE_DEVICE,
                           .dstDevice = (CUdeviceptr)(uintptr_t)p.data[i],
                           .dstPitch = (size_t)p.stride[i],
                           .WidthInBytes = row[i],
                           .Height = h[i]};
        ok = gpu.f->cuMemcpy2D(&m) == CUDA_SUCCESS;
        src += row[i] * h[i];
    }
    vc_pop(&gpu);
    return ok;
}

static char *pool_frames(const Clip *c, VmafxFramePool *pool, VmafxFrame **frames)
{
    const size_t frame = vt_frame_bytes(&c->desc);
    for (unsigned k = 0; k < 2u * FRAMES; k++) {
        mu_assert("acquire", vmafx_frame_pool_acquire(pool, &frames[k], NULL) == VMAFX_OK);
        const uint8_t *const src = (k & 1u ? c->dist : c->ref) + (k / 2u) * frame;
        mu_assert("fill", fill_pool_frame(c, frames[k], src));
    }
    VmafxFrame *extra = NULL;
    VmafxError *error = NULL;
    mu_assert("exhausted", vmafx_frame_pool_acquire(pool, &extra, &error) == VMAFX_E_BUSY &&
                               vt_failed(&error, VMAFX_E_BUSY, "pool", VMAFX_SUBJECT_FRAME));
    return NULL;
}

static char *test_pool(void)
{
    const Clip c = clip8;
    VmafxModel *model = NULL;
    VmafxFramePool *pool = NULL;
    VmafxFrame *frames[2u * FRAMES];
    memset((void *)frames, 0, sizeof(frames));
    mu_assert("model", vmafx_model_load(NULL, MODEL, &model, NULL) == VMAFX_OK);
    mu_assert("pool",
              vmafx_frame_pool_create(gpu.device, &c.desc, 2u * FRAMES, &pool, NULL) == VMAFX_OK);
    char *msg = pool_frames(&c, pool, frames);
    msg = msg ? msg : score_and_compare(&c, model, frames);
    VmafxFence fence = VMAFX_FENCE_INIT;
    const bool fenced =
        vmafx_frame_release_fence(frames[0], VMAFX_FENCE_CUDA_EVENT, &fence, NULL) == VMAFX_OK;
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
    mu_assert("pool frame released after its readers", released);
    mu_assert("a frame returns to the pool", reused);
    return NULL;
}

/* ---- Admission ------------------------------------------------------------------------ */

/* An extractor without a CUDA twin runs on the CPU: it would need a host
 * copy of the imported frame and is named. */
static char *test_admission(void)
{
    const Clip c = clip8;
    VcPlanes p;
    memset(&p, 0, sizeof(p));
    mu_assert("upload", vc_upload(&gpu, &c.desc, c.ref, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 0u, &p));
    VmafxContext *context = NULL;
    mu_assert("context",
              vmafx_context_create(NULL, &context, NULL) == VMAFX_OK &&
                  vmafx_context_use_device(context, gpu.device, NULL) == VMAFX_OK &&
                  vmafx_context_use_feature(context, "psnr", NULL, NULL) == VMAFX_OK &&
                  vmafx_context_use_feature(context, "delta_e_itp", NULL, NULL) == VMAFX_OK);
    const VmafxFrameImport imp = vc_import_desc(&c.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &p);
    VmafxFrame *frame = NULL;
    VmafxError *error = NULL;
    const VmafxStatus status =
        vmafx_context_import_frame(context, gpu.device, &imp, "main", &frame, &error);
    const char *const msg = vmafx_error_message(error);
    const bool named = status == VMAFX_E_NOTSUP && msg && strstr(msg, "delta_e_itp (cpu") &&
                       !strstr(msg, "psnr") && strstr(msg, "main: backend cuda device 0");
    if (!named) {
        (void)fprintf(stderr, "\n  status %d: %s\n", (int)status, msg ? msg : "(none)");
    }
    vmafx_error_free(error);
    (void)vmafx_context_destroy(context, NULL);
    vc_free(&gpu, &p);
    mu_assert("the CPU extractor named, the CUDA twin admitted", named && frame == NULL);
    return NULL;
}

/* ---- One import, two contexts ------------------------------------------------------------ */

typedef struct Shared {
    Clip clip;
    VmafxModel *models[2];
    VmafxContext *contexts[2];
    VcPlanes planes[2u * FRAMES];
    VmafxFence event; /* CUDA_EVENT release fence of the last reference frame */
    VmafxFence host;  /* HOST release fence of the same frame */
} Shared;

static char *shared_submit(Shared *s)
{
    const size_t frame = vt_frame_bytes(&s->clip.desc);
    const uint64_t attempts = vmafx_test_import_attempts();
    for (unsigned i = 0; i < FRAMES; i++) {
        VmafxFrame *pair[2] = {NULL, NULL};
        for (unsigned side = 0; side < 2u; side++) {
            VcPlanes *const p = &s->planes[(size_t)2u * i + side];
            const uint8_t *const src = (side ? s->clip.dist : s->clip.ref) + i * frame;
            mu_assert("upload",
                      vc_upload(&gpu, &s->clip.desc, src, VMAFX_PIXEL_FORMAT_NV12, 0u, 16u, p));
            VmafxFrameImport imp = vc_import_desc(&s->clip.desc, VMAFX_PIXEL_FORMAT_NV12, 8u, p);
            CUevent ev = NULL;
            imp.acquire = vc_record(&gpu, &ev);
            mu_assert("import",
                      vmafx_frame_import(gpu.device, &imp, &pair[side], NULL) == VMAFX_OK);
            (void)gpu.f->cuEventDestroy(ev);
        }
        (void)vmafx_fence_destroy(&s->event, NULL);
        (void)vmafx_fence_destroy(&s->host, NULL);
        mu_assert("fences", vmafx_frame_release_fence(pair[0], VMAFX_FENCE_CUDA_EVENT, &s->event,
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
        vc_free(&gpu, &s->planes[k]);
    }
}

static char *test_one_import_two_contexts(void)
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

/* ---- Host copies ------------------------------------------------------------------------ */

/* The planted host copy (VMAFX_TEST_FORCE_HOST_COPY) is counted, one per
 * plane, and the scores do not change; without it the count stays 0. */
static char *test_host_copy_counter(void)
{
    mu_assert("no host copy so far", vmafx_test_host_copies() == 0u);
    const Clip c = clip8;
    VcPlanes p;
    memset(&p, 0, sizeof(p));
    mu_assert("upload", vc_upload(&gpu, &c.desc, c.ref, VMAFX_PIXEL_FORMAT_YUV420P, 0u, 8u, &p));
    const VmafxFrameImport imp = vc_import_desc(&c.desc, VMAFX_PIXEL_FORMAT_YUV420P, 8u, &p);
    VmafxFrame *frame = NULL;
    vmafx_test_set_switches(VMAFX_TEST_FORCE_HOST_COPY);
    const VmafxStatus status = vmafx_frame_import(gpu.device, &imp, &frame, NULL);
    vmafx_test_set_switches(0u);
    vmafx_frame_unref(frame);
    const uint64_t copies = vmafx_test_host_copies();
    vmafx_test_reset_counters();
    vc_free(&gpu, &p);
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
GUARDED(test_device_external)
GUARDED(test_context_device)
GUARDED(test_import_refusals)
GUARDED(test_host_acquire_busy)
GUARDED(test_arrays)
GUARDED(test_unaligned_allow_copy)
GUARDED(test_pool)
GUARDED(test_admission)
GUARDED(test_one_import_two_contexts)
GUARDED(test_host_copy_counter)

char *run_tests(void)
{
    vmafx_test_reset_counters();
    have_gpu = vc_open(&gpu) && clip_make(&clip8, 8u);
    static const MuTest tests[] = {
        MU_TEST(test_device_info_g),
        MU_TEST(test_device_create_refusals_g),
        MU_TEST(test_device_external_g),
        MU_TEST(test_context_device_g),
        MU_TEST(test_import_refusals_g),
        MU_TEST(test_host_acquire_busy_g),
        MU_TEST(test_arrays_g),
        MU_TEST(test_unaligned_allow_copy_g),
        MU_TEST(test_pool_g),
        MU_TEST(test_admission_g),
        MU_TEST(test_one_import_two_contexts_g),
        MU_TEST(test_host_copy_counter_g),
    };
    char *const msg = mu_run_table(tests, MU_TABLE_LEN(tests));
    clip_free(&clip8);
    vc_close(&gpu);
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
