/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * OpenGL textures on the VMAFx HIP lane (RC4 WP3, #2238, ADR-2132): frames
 * rendered into GL textures (NV12 as an R8 luma and an RG8 chroma texture,
 * P010 as R16 and RG16: the layouts a screen-capture pipeline renders) and
 * imported on a HIP device through EGL's dma-buf export, with a GL sync
 * object as their acquire fence, score bit for bit as the same frames
 * uploaded from the host. The HIP runtime's own GL interop is not involved
 * (ADR-2092: unreadable on the pinned ROCm 10.1).
 *
 * Also: a texture the driver exports tiled is refused without
 * VMAFX_IMPORT_ALLOW_COPY and copied on the GPU with it; the import leaves
 * the producer's GL state as it found it; an import without an EGL context
 * current on the thread is refused naming the memory kind; a context of
 * another GPU is refused.
 *
 * Headless: a GL context on the EGL device whose render node is the HIP
 * device's GPU (vmafx_egl_test_util.h; surfaceless), EGL and GL resolved at
 * run time. Skips (77) without a HIP device or without such an EGL device.
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
#include "vmafx_egl_test_util.h"
#include "vmafx_hip_test_util.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define W 640u
#define H 360u
#define FRAMES 3u
#define MODEL "vmaf_v1.0.16_3d0h"
/* Attempts at a comparison that differs (the gfx1036's lost commands,
 * T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01). */
#define ATTEMPTS 3u

static VhGpu gpu;
static bool have_gpu;
static Vegl egl;
static bool have_gl;

/* ---- Frames ------------------------------------------------------------------------- */

/* Render one planar frame (`d`) as textures; `pix_fmt` and `shift` say how the
 * producer lays it out (NV12: 8 bits, shift 0; P010: 10 bits in 16, shift 6). */
static bool render(const VmafxFrameDesc *d, unsigned shift, const uint8_t *planar, VeglFrame *f)
{
    uint8_t *const semi = malloc(vt_frame_bytes(d));
    if (!semi) {
        return false;
    }
    vt_to_semiplanar(d, planar, shift, semi);
    const bool ok = vegl_upload(&egl, f, d->w, d->h, d->bpc > 8u ? 2u : 1u, semi);
    free(semi);
    return ok;
}

static VmafxFrameImport gl_import(const VmafxFrameDesc *d, const VeglFrame *f, uint32_t flags)
{
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    imp.memory = VMAFX_MEMORY_GL_TEXTURE;
    imp.pix_fmt = d->bpc > 8u ? VMAFX_PIXEL_FORMAT_P010 : VMAFX_PIXEL_FORMAT_NV12;
    imp.bpc = d->bpc;
    imp.w = d->w;
    imp.h = d->h;
    imp.n_planes = 2u;
    imp.flags = flags;
    imp.plane[0].handle = f->tex[0];
    imp.plane[1].handle = f->tex[1];
    imp.acquire.kind = VMAFX_FENCE_GL_SYNC;
    imp.acquire.handle = (uintptr_t)f->sync;
    return imp;
}

/* ---- Sessions ------------------------------------------------------------------------- */

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

static char *run_gl(VmafxContext *context, const VmafxFrameDesc *d, unsigned shift,
                    const uint8_t *ref, const uint8_t *dist, VeglFrame *frames)
{
    const size_t frame = vt_frame_bytes(d);
    for (unsigned i = 0; i < FRAMES; i++) {
        VmafxFrame *pair[2] = {NULL, NULL};
        for (unsigned s = 0; s < 2u; s++) {
            VeglFrame *const f = &frames[2u * i + s];
            mu_assert("render", render(d, shift, (s ? dist : ref) + i * frame, f));
            const VmafxFrameImport imp = gl_import(d, f, VMAFX_IMPORT_ALLOW_COPY);
            mu_assert("import", vmafx_context_import_frame(context, gpu.device, &imp,
                                                           s ? "reference" : "main", &pair[s],
                                                           NULL) == VMAFX_OK);
        }
        mu_assert("submit", vmafx_submit(context, pair[0], pair[1], i, NULL) == VMAFX_OK);
    }
    mu_assert("flush", vmafx_flush(context, NULL) == VMAFX_OK);
    return NULL;
}

static VmafxContext *run_host(const VmafxFrameDesc *d, uint8_t *ref, uint8_t *dist,
                              VmafxModel *model)
{
    VmafxContext *const context = model_context(model);
    const size_t frame = vt_frame_bytes(d);
    bool ok = context != NULL;
    for (unsigned i = 0; i < FRAMES && ok; i++) {
        VmafxFrame *r = vt_wrap_frame(d, ref + i * frame, NULL);
        VmafxFrame *x = vt_wrap_frame(d, dist + i * frame, NULL);
        ok = vmafx_submit(context, r, x, i, NULL) == VMAFX_OK;
    }
    return ok && vmafx_flush(context, NULL) == VMAFX_OK ? context : NULL;
}

/* One attempt: GL frames and host frames, compared; `*differing` counts. */
static char *compare_gl_once(const VmafxFrameDesc *d, unsigned shift, uint8_t *ref, uint8_t *dist,
                             VmafxModel *model, unsigned long *differing)
{
    VeglFrame frames[2u * FRAMES];
    memset(frames, 0, sizeof(frames));
    VmafxContext *const imported = model_context(model);
    char *msg = imported ? run_gl(imported, d, shift, ref, dist, frames) : "context";
    VmafxContext *const host = msg ? NULL : run_host(d, ref, dist, model);
    unsigned long compared = 0;
    msg = msg ?
              msg :
              (host && vc_compare(host, imported, FRAMES, &compared, differing) ? NULL : "compare");
    (void)fprintf(stderr, "[%lu values, %lu differing] ", compared, *differing);
    if (imported) {
        (void)vmafx_context_destroy(imported, NULL);
    }
    if (host) {
        (void)vmafx_context_destroy(host, NULL);
    }
    for (unsigned k = 0; k < 2u * FRAMES; k++) {
        vegl_free_frame(&egl, &frames[k]);
    }
    return msg;
}

/* GL frames of `bpc` bits score as the host frames. */
static char *check_gl_scores(uint32_t bpc, unsigned shift)
{
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, bpc, W, H);
    const size_t frame = vt_frame_bytes(&d);
    uint8_t *const ref = malloc(frame * FRAMES);
    uint8_t *const dist = malloc(frame * FRAMES);
    VmafxModel *model = NULL;
    char *msg =
        ref && dist && vmafx_model_load(NULL, MODEL, &model, NULL) == VMAFX_OK ? NULL : "setup";
    for (unsigned i = 0; i < FRAMES && !msg; i++) {
        vt_fill(&d, ref + i * frame, i);
        vt_fill(&d, dist + i * frame, i + 9u);
    }
    unsigned long differing = 1;
    for (unsigned a = 0; a < ATTEMPTS && !msg && differing != 0u; a++) {
        differing = 0;
        msg = compare_gl_once(&d, shift, ref, dist, model, &differing);
    }
    vmafx_model_unref(model);
    free(ref);
    free(dist);
    mu_assert_msg(msg);
    mu_assert("GL frames score as host frames", differing == 0u);
    return NULL;
}

static char *test_gl_textures_nv12(void)
{
    if (!have_gl) {
        mu_skipped = 1;
        return NULL;
    }
    vmafx_test_reset_counters();
    char *const msg = check_gl_scores(8u, 0u);
    mu_assert_msg(msg);
    mu_assert("no host copy", vmafx_test_host_copies() == 0u);
    mu_assert("converted on the device", vmafx_test_conversions() > 0u);
    return NULL;
}

static char *test_gl_textures_p010(void)
{
    if (!have_gl) {
        mu_skipped = 1;
        return NULL;
    }
    vmafx_test_reset_counters();
    char *const msg = check_gl_scores(10u, 6u);
    mu_assert_msg(msg);
    mu_assert("no host copy", vmafx_test_host_copies() == 0u);
    return NULL;
}

/* ---- Refusals and the producer's state ---------------------------------------------------- */

static VmafxStatus import_once(const VmafxFrameDesc *d, const VeglFrame *f, uint32_t flags,
                               VmafxError **error, VmafxFrame **frame)
{
    const VmafxFrameImport imp = gl_import(d, f, flags);
    return vmafx_frame_import(gpu.device, &imp, frame, error);
}

/* A texture the driver exports tiled is not read in place: without
 * VMAFX_IMPORT_ALLOW_COPY the import is refused naming the flag; with it the
 * GPU copy makes a linear dma-buf and the import succeeds. A driver that
 * exports linear needs no copy and skips the refusal half. */
static char *test_gl_tiled_needs_allow_copy(void)
{
    if (!have_gl) {
        mu_skipped = 1;
        return NULL;
    }
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8u, W, H);
    uint8_t *const data = malloc(vt_frame_bytes(&d));
    VeglFrame f;
    memset(&f, 0, sizeof(f));
    mu_assert("frame", data != NULL);
    vt_fill(&d, data, 3u);
    mu_assert("render", render(&d, 0u, data, &f));
    egl.finish();
    VmafxError *error = NULL;
    VmafxFrame *frame = NULL;
    const VmafxStatus plain = import_once(&d, &f, 0u, &error, &frame);
    const bool named = error != NULL && strstr(vmafx_error_message(error), "ALLOW_COPY") != NULL;
    vmafx_error_free(error);
    error = NULL;
    if (plain == VMAFX_OK) {
        vmafx_frame_unref(frame);
        (void)fprintf(stderr, "[the driver exports these textures linear: no copy needed] ");
        frame = NULL;
    }
    const VmafxStatus copy = import_once(&d, &f, VMAFX_IMPORT_ALLOW_COPY, &error, &frame);
    vmafx_error_free(error);
    if (frame) {
        vmafx_frame_unref(frame);
    }
    vegl_free_frame(&egl, &f);
    free(data);
    mu_assert("tiled import refused without the flag, naming it",
              plain == VMAFX_OK || (plain == VMAFX_E_NOTSUP && named));
    mu_assert("imported with the flag", copy == VMAFX_OK);
    return NULL;
}

/* The import changes no state of the producer's GL context: framebuffer
 * binding, scissor test. */
static char *test_gl_state_is_restored(void)
{
    if (!have_gl) {
        mu_skipped = 1;
        return NULL;
    }
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8u, W, H);
    uint8_t *const data = malloc(vt_frame_bytes(&d));
    VeglFrame f;
    memset(&f, 0, sizeof(f));
    mu_assert("frame", data != NULL);
    vt_fill(&d, data, 5u);
    mu_assert("render", render(&d, 0u, data, &f));
    egl.finish();
    unsigned fbo = 0;
    egl.gen_framebuffers(1, &fbo);
    egl.bind_framebuffer(VGL_FRAMEBUFFER, fbo);
    egl.enable(VGL_SCISSOR_TEST);
    VmafxError *error = NULL;
    VmafxFrame *frame = NULL;
    const VmafxStatus status = import_once(&d, &f, VMAFX_IMPORT_ALLOW_COPY, &error, &frame);
    int bound = -1;
    egl.get_integer(VGL_DRAW_FRAMEBUFFER_BINDING, &bound);
    const bool scissor = egl.is_enabled(VGL_SCISSOR_TEST) != 0u;
    egl.disable(VGL_SCISSOR_TEST);
    egl.bind_framebuffer(VGL_FRAMEBUFFER, 0u);
    egl.delete_framebuffers(1, &fbo);
    vmafx_error_free(error);
    if (frame) {
        vmafx_frame_unref(frame);
    }
    vegl_free_frame(&egl, &f);
    free(data);
    mu_assert("imported", status == VMAFX_OK);
    mu_assert("framebuffer binding restored", bound == (int)fbo);
    mu_assert("scissor test restored", scissor);
    return NULL;
}

/* Without an EGL context current on the thread, a GL import is refused
 * naming the memory kind. */
static char *test_gl_needs_egl_context(void)
{
    if (!have_gl) {
        mu_skipped = 1;
        return NULL;
    }
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8u, W, H);
    uint8_t *const data = malloc(vt_frame_bytes(&d));
    VeglFrame f;
    memset(&f, 0, sizeof(f));
    mu_assert("frame", data != NULL);
    vt_fill(&d, data, 1u);
    const bool rendered = render(&d, 0u, data, &f);
    egl.finish();
    (void)egl.make_current(egl.dpy, NULL, NULL, NULL);
    VmafxError *error = NULL;
    VmafxFrame *frame = NULL;
    /* The GL sync cannot be checked without a context either: no acquire. */
    VmafxFrameImport imp = gl_import(&d, &f, VMAFX_IMPORT_ALLOW_COPY);
    imp.acquire = (VmafxFence)VMAFX_FENCE_INIT;
    const VmafxStatus status = vmafx_frame_import(gpu.device, &imp, &frame, &error);
    const bool named = vt_failed(&error, VMAFX_E_INVALID, "desc.memory", VMAFX_SUBJECT_PARAMETER);
    (void)egl.make_current(egl.dpy, NULL, NULL, egl.ctx);
    vegl_free_frame(&egl, &f);
    free(data);
    mu_assert("refused", rendered && status == VMAFX_E_INVALID && named && frame == NULL);
    return NULL;
}

char *run_tests(void)
{
    vmafx_test_reset_counters();
    have_gpu = vh_open(&gpu);
    have_gl = have_gpu && gpu.pci_bus_id[0] != '\0' && vegl_open(&egl, gpu.pci_bus_id);
    if (!have_gl) {
        (void)fprintf(stderr, "[no EGL context on the HIP device's GPU: skipped] ");
    }
    static const MuTest tests[] = {
        MU_TEST(test_gl_needs_egl_context), MU_TEST(test_gl_textures_nv12),
        MU_TEST(test_gl_textures_p010),     MU_TEST(test_gl_tiled_needs_allow_copy),
        MU_TEST(test_gl_state_is_restored),
    };
    char *const msg = mu_run_table(tests, MU_TABLE_LEN(tests));
    vegl_drop(&egl);
    vh_close(&gpu);
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
