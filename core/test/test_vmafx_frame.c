/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VMAFx devices, host frames and submission (ADR-1852, RC4 WP2): the CPU
 * device skeleton, allocated and borrowed host frames, the reference a submit
 * consumes on every path, frame retention, and one frame scored by two
 * contexts with different options without a copy (PR #2185).
 *
 * Failing first: the functions do not exist on the WP1 base. Measured with
 * planted defects on this branch: a submit that keeps the caller's
 * references on a refused path, or one that takes a reference of its own
 * instead of consuming the caller's, fails test_submit_and_flush (every frame
 * released exactly once).
 */

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/vmafx.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

enum { W = 176, H = 144, N_FRAMES = 6 };

static VmafxContext *psnr_context(void)
{
    VmafxContext *context = NULL;
    if (vmafx_context_create(NULL, &context, NULL) != VMAFX_OK) {
        return NULL;
    }
    if (vmafx_context_use_feature(context, "psnr", NULL, NULL) != VMAFX_OK) {
        (void)vmafx_context_destroy(context, NULL);
        return NULL;
    }
    return context;
}

/* ---- Devices ------------------------------------------------------------------------ */

static char *test_device_skeleton(void)
{
    VmafxDevice *device = NULL;
    mu_assert("CPU", vmafx_device_create(NULL, &device, NULL) == VMAFX_OK &&
                         vmafx_device_backend(device) == VMAFX_BACKEND_CPU);
    mu_assert("ref", vmafx_device_ref(device) == device);
    vmafx_device_unref(device);
    vmafx_device_unref(device);
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.backend = VMAFX_BACKEND_CUDA;
    VmafxError *error = NULL;
    mu_assert("not in this release",
              vmafx_device_create(&desc, &device, &error) == VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "desc.backend", VMAFX_SUBJECT_BACKEND));
    desc.backend = VMAFX_BACKEND_CPU;
    desc.index = 3;
    mu_assert("no CPU 3",
              vmafx_device_create(&desc, &device, &error) == VMAFX_E_NOTFOUND &&
                  vt_failed(&error, VMAFX_E_NOTFOUND, "desc.index", VMAFX_SUBJECT_DEVICE));
    desc.struct_size = 4;
    mu_assert("short desc", vmafx_device_create(&desc, &device, &error) == VMAFX_E_ABI &&
                                vt_failed(&error, VMAFX_E_ABI, "desc", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL out", vmafx_device_create(NULL, NULL, NULL) == VMAFX_E_INVALID);
    mu_assert("NULL", vmafx_device_backend(NULL) == VMAFX_BACKEND_CPU && !vmafx_device_ref(NULL));
    vmafx_device_unref(NULL);
    return NULL;
}

/* ---- Host frames --------------------------------------------------------------------- */

static char *test_create_host_geometry(void)
{
    VmafxDevice *device = NULL;
    mu_assert("device", vmafx_device_create(NULL, &device, NULL) == VMAFX_OK);
    const VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 10, 65, 33);
    VmafxFrame *frame = NULL;
    mu_assert("create", vmafx_frame_create_host(device, &desc, &frame, NULL) == VMAFX_OK);
    vmafx_device_unref(device); /* the frame holds its own reference */
    VmafxFramePlanes p = VMAFX_FRAME_PLANES_INIT;
    mu_assert("planes", vmafx_frame_planes(frame, &p, NULL) == VMAFX_OK);
    mu_assert("format", p.pix_fmt == desc.pix_fmt && p.bpc == 10 && p.n_planes == 3);
    mu_assert("odd 4:2:0 chroma rounds up", p.w[0] == 65 && p.h[0] == 33 && p.w[1] == 33 &&
                                                p.h[1] == 17 && p.w[2] == 33 && p.h[2] == 17);
    mu_assert("rows fit", p.stride[0] >= 130u && p.stride[1] >= 66u && p.data[0] && p.data[2]);
    vmafx_frame_unref(frame);
    const VmafxFrameDesc luma = vt_desc(VMAFX_PIXEL_FORMAT_YUV400P, 8, 16, 16);
    mu_assert("luma only", vmafx_frame_create_host(NULL, &luma, &frame, NULL) == VMAFX_OK &&
                               vmafx_frame_planes(frame, &p, NULL) == VMAFX_OK && p.n_planes == 1 &&
                               p.data[1] == NULL && p.w[1] == 0);
    vmafx_frame_unref(frame);
    return NULL;
}

typedef struct DescCase {
    VmafxFrameDesc desc;
    VmafxStatus status;
    const char *subject;
} DescCase;

static char *test_create_host_failures(void)
{
    const DescCase cases[] = {
        {vt_desc(0, 8, 16, 16), VMAFX_E_INVALID, "desc.pix_fmt"},
        {vt_desc(5, 8, 16, 16), VMAFX_E_INVALID, "desc.pix_fmt"},
        {vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 7, 16, 16), VMAFX_E_INVALID, "desc.bpc"},
        {vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 17, 16, 16), VMAFX_E_INVALID, "desc.bpc"},
        {vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, 0, 16), VMAFX_E_INVALID, "desc.w"},
        {vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, 16, 0), VMAFX_E_INVALID, "desc.h"},
        {vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, 40000, 16), VMAFX_E_INVALID, "desc"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        VmafxFrame *frame = NULL;
        VmafxError *error = NULL;
        const VmafxStatus status = vmafx_frame_create_host(NULL, &cases[i].desc, &frame, &error);
        const uint32_t kind = VMAFX_SUBJECT_PARAMETER;
        mu_assert("refused and named", status == cases[i].status && frame == NULL &&
                                           vt_failed(&error, status, cases[i].subject, kind));
    }
    VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, 16, 16);
    desc.struct_size = 8;
    VmafxFrame *frame = NULL;
    VmafxError *error = NULL;
    mu_assert("short desc", vmafx_frame_create_host(NULL, &desc, &frame, &error) == VMAFX_E_ABI &&
                                vt_failed(&error, VMAFX_E_ABI, "desc", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL desc", vmafx_frame_create_host(NULL, NULL, &frame, &error) == VMAFX_E_INVALID &&
                               vt_failed(&error, VMAFX_E_INVALID, "desc", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL out", vmafx_frame_create_host(NULL, &desc, NULL, NULL) == VMAFX_E_INVALID);
    return NULL;
}

static char *test_wrap_host(void)
{
    const VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    uint8_t *data = malloc(vt_frame_bytes(&desc));
    unsigned released = 0;
    VmafxFrame *frame = vt_wrap_frame(&desc, data, &released);
    mu_assert("wrapped", frame != NULL);
    VmafxFramePlanes p = VMAFX_FRAME_PLANES_INIT;
    mu_assert("borrowed, no copy", vmafx_frame_planes(frame, &p, NULL) == VMAFX_OK &&
                                       p.data[0] == data && p.stride[0] == W);
    mu_assert("second reference", vmafx_frame_ref(frame) == frame);
    vmafx_frame_unref(frame);
    mu_assert("not released while referenced", released == 0);
    vmafx_frame_unref(frame);
    mu_assert("released once by the last reference", released == 1);
    free(data);
    mu_assert("NULL", vmafx_frame_ref(NULL) == NULL);
    vmafx_frame_unref(NULL);
    return NULL;
}

static char *test_wrap_host_failures(void)
{
    const VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    uint8_t plane[W * H];
    unsigned released = 0;
    VmafxHostPlanes p = VMAFX_HOST_PLANES_INIT;
    p.data[0] = plane;
    p.stride[0] = W;
    p.stride[1] = p.stride[2] = W / 2;
    p.release = vt_count_release;
    p.user = &released;
    VmafxFrame *frame = NULL;
    VmafxError *error = NULL;
    mu_assert("missing chroma",
              vmafx_frame_wrap_host(NULL, &desc, &p, &frame, &error) == VMAFX_E_INVALID);
    mu_assert("named", vt_failed(&error, VMAFX_E_INVALID, "planes.data[1]", VMAFX_SUBJECT_PLANE));
    p.data[1] = p.data[2] = plane;
    p.stride[0] = W - 1;
    mu_assert("short rows",
              vmafx_frame_wrap_host(NULL, &desc, &p, &frame, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "planes.stride[0]", VMAFX_SUBJECT_PLANE));
    const VmafxFrameDesc luma = vt_desc(VMAFX_PIXEL_FORMAT_YUV400P, 8, W, H);
    p.stride[0] = W;
    p.data[1] = p.data[2] = NULL;
    mu_assert("luma only needs no chroma",
              vmafx_frame_wrap_host(NULL, &luma, &p, &frame, NULL) == VMAFX_OK);
    vmafx_frame_unref(frame);
    mu_assert("a refused wrap never calls release; the frame's last unref does", released == 1);
    mu_assert("NULL planes",
              vmafx_frame_wrap_host(NULL, &desc, NULL, &frame, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "planes", VMAFX_SUBJECT_PARAMETER));
    return NULL;
}

static char *test_frame_planes_failures(void)
{
    const VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV444P, 8, 16, 16);
    VmafxFrame *frame = NULL;
    mu_assert("create", vmafx_frame_create_host(NULL, &desc, &frame, NULL) == VMAFX_OK);
    VmafxFramePlanes p = VMAFX_FRAME_PLANES_INIT;
    VmafxError *error = NULL;
    mu_assert("NULL frame",
              vmafx_frame_planes(NULL, &p, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "frame", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL out", vmafx_frame_planes(frame, NULL, &error) == VMAFX_E_INVALID &&
                              vt_failed(&error, VMAFX_E_INVALID, "out", VMAFX_SUBJECT_PARAMETER));
    p.struct_size = 2;
    mu_assert("short out", vmafx_frame_planes(frame, &p, &error) == VMAFX_E_ABI &&
                               vt_failed(&error, VMAFX_E_ABI, "out", VMAFX_SUBJECT_PARAMETER));
    vmafx_frame_unref(frame);
    return NULL;
}

/* ---- Submission ------------------------------------------------------------------------ */

/* Frames over `data` (N_FRAMES tightly packed frames) counting releases. */
typedef struct Clip {
    VmafxFrameDesc desc;
    uint8_t *ref;
    uint8_t *dist;
    unsigned released;
} Clip;

static bool clip_open(Clip *clip, uint32_t bpc)
{
    clip->desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, bpc, W, H);
    const size_t bytes = vt_frame_bytes(&clip->desc);
    clip->ref = malloc(bytes * N_FRAMES);
    clip->dist = malloc(bytes * N_FRAMES);
    clip->released = 0;
    if (!clip->ref || !clip->dist) {
        return false;
    }
    for (unsigned i = 0; i < N_FRAMES; i++) {
        vt_fill(&clip->desc, clip->ref + i * bytes, i);
        vt_fill(&clip->desc, clip->dist + i * bytes, i + 100u);
    }
    return true;
}

static void clip_close(Clip *clip)
{
    free(clip->ref);
    free(clip->dist);
}

static VmafxFrame *clip_frame(Clip *clip, bool reference, unsigned i)
{
    uint8_t *base = reference ? clip->ref : clip->dist;
    return vt_wrap_frame(&clip->desc, base + i * vt_frame_bytes(&clip->desc), &clip->released);
}

static VmafxStatus submit_index(VmafxContext *context, Clip *clip, unsigned frame, uint64_t index,
                                VmafxError **error)
{
    return vmafx_submit(context, clip_frame(clip, true, frame), clip_frame(clip, false, frame),
                        index, error);
}

static char *test_submit_and_flush(void)
{
    Clip clip;
    mu_assert("clip", clip_open(&clip, 8));
    VmafxContext *context = psnr_context();
    for (unsigned i = 0; i < N_FRAMES; i++) {
        mu_assert("submit", submit_index(context, &clip, i, i, NULL) == VMAFX_OK);
    }
    mu_assert("flush", vmafx_flush(context, NULL) == VMAFX_OK);
    VmafxError *error = NULL;
    mu_assert("second flush",
              vmafx_flush(context, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "context", VMAFX_SUBJECT_CONTEXT));
    mu_assert("submit after flush",
              submit_index(context, &clip, 0, N_FRAMES, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "context", VMAFX_SUBJECT_CONTEXT));
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    mu_assert("every frame released once", clip.released == 2u * (N_FRAMES + 1u));
    mu_assert("NULL flush", vmafx_flush(NULL, NULL) == VMAFX_E_INVALID);
    clip_close(&clip);
    return NULL;
}

static char *test_submit_failures_consume_frames(void)
{
    Clip clip;
    mu_assert("clip", clip_open(&clip, 8));
    VmafxContext *context = psnr_context();
    VmafxError *error = NULL;
    mu_assert("NULL context",
              vmafx_submit(NULL, clip_frame(&clip, true, 0), clip_frame(&clip, false, 0), 0,
                           &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "context", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL reference",
              vmafx_submit(context, NULL, clip_frame(&clip, false, 0), 0, &error) ==
                      VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "reference", VMAFX_SUBJECT_PARAMETER));
    VmafxFrame *same = clip_frame(&clip, true, 0);
    mu_assert("one reference for both inputs",
              vmafx_submit(context, same, same, 0, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "distorted", VMAFX_SUBJECT_FRAME));
    mu_assert("consumed on every path", clip.released == 4);
    const uint64_t too_far = (uint64_t)UINT_MAX + 1u;
    mu_assert("range", submit_index(context, &clip, 0, too_far, &error) == VMAFX_E_RANGE &&
                           vt_failed(&error, VMAFX_E_RANGE, "index", VMAFX_SUBJECT_FRAME));
    mu_assert("first", submit_index(context, &clip, 0, 5, NULL) == VMAFX_OK);
    mu_assert("not increasing",
              submit_index(context, &clip, 1, 5, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "index", VMAFX_SUBJECT_FRAME));
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    mu_assert("all released", clip.released == 10);
    clip_close(&clip);
    return NULL;
}

static char *test_submit_geometry_named(void)
{
    Clip clip;
    mu_assert("clip", clip_open(&clip, 8));
    VmafxContext *context = psnr_context();
    const VmafxFrameDesc small = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, 64, 64);
    VmafxFrame *other = NULL;
    mu_assert("other", vmafx_frame_create_host(NULL, &small, &other, NULL) == VMAFX_OK);
    VmafxError *error = NULL;
    mu_assert("pair differs",
              vmafx_submit(context, clip_frame(&clip, true, 0), other, 0, &error) ==
                      VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "distorted", VMAFX_SUBJECT_FRAME));
    mu_assert("first", submit_index(context, &clip, 0, 0, NULL) == VMAFX_OK);
    mu_assert("other", vmafx_frame_create_host(NULL, &small, &other, NULL) == VMAFX_OK);
    VmafxFrame *other2 = vmafx_frame_ref(other);
    mu_assert("differs from the first frame",
              vmafx_submit(context, other, other2, 1, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "reference", VMAFX_SUBJECT_FRAME));
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    mu_assert("all released", clip.released == 3);
    clip_close(&clip);
    return NULL;
}

/* ---- Retention (ADR-1478) -------------------------------------------------------------------- */

static char *test_retention_keeps_n_minus_2(void)
{
    Clip clip;
    mu_assert("clip", clip_open(&clip, 8));
    VmafxContext *context = psnr_context();
    VmafxOptions *five = NULL;
    mu_assert("option", vmafx_options_set(&five, "motion_five_frame_window", "true", NULL) == 0);
    mu_assert("motion", vmafx_context_use_feature(context, "motion", five, NULL) == VMAFX_OK);
    vmafx_options_free(five);
    mu_assert("retention", vmafx_context_frame_retention(context) == 2);
    const size_t bytes = vt_frame_bytes(&clip.desc);
    unsigned ref_released[3] = {0, 0, 0};
    for (unsigned i = 0; i < 3u; i++) {
        VmafxFrame *ref = vt_wrap_frame(&clip.desc, clip.ref + i * bytes, &ref_released[i]);
        mu_assert("submit",
                  vmafx_submit(context, ref, clip_frame(&clip, false, i), i, NULL) == VMAFX_OK);
    }
    /* The caller holds no reference: frames 1 and 2 are frames n-2 and n-1 of
     * the next submit, so the context still holds them. */
    mu_assert("n-1 and n-2 retained", ref_released[1] == 0 && ref_released[2] == 0);
    mu_assert("flush", vmafx_flush(context, NULL) == VMAFX_OK);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    mu_assert("all released once", ref_released[0] == 1 && ref_released[1] == 1 &&
                                       ref_released[2] == 1 && clip.released == 3u);
    clip_close(&clip);
    return NULL;
}

/* ---- One frame, two contexts (device-targeted scoring, PR #2185) ---------------------------- */

/* A context scoring vmaf_v0.6.1, its ADM at normalised viewing distance `nvd`. */
static VmafxContext *vmaf_context(const char *nvd, VmafxModel **model)
{
    VmafxContext *context = NULL;
    VmafxOptions *options = NULL;
    *model = NULL;
    if (vmafx_context_create(NULL, &context, NULL) != VMAFX_OK ||
        vmafx_model_load(NULL, "vmaf_v0.6.1", model, NULL) != VMAFX_OK ||
        vmafx_options_set(&options, "adm_norm_view_dist", nvd, NULL) != VMAFX_OK ||
        vmafx_model_override_feature(*model, "adm", options, NULL) != VMAFX_OK ||
        vmafx_context_use_model(context, *model, NULL) != VMAFX_OK) {
        vmafx_options_free(options);
        return NULL;
    }
    vmafx_options_free(options);
    return context;
}

/* Score `n` frames of `clip` in `context` alone, from copies. */
static bool score_alone(VmafxContext *context, Clip *clip, unsigned n)
{
    const size_t bytes = vt_frame_bytes(&clip->desc);
    bool ok = true;
    for (unsigned i = 0; i < n && ok; i++) {
        VmafxFrame *ref = vt_copy_frame(&clip->desc, clip->ref + i * bytes);
        VmafxFrame *dist = vt_copy_frame(&clip->desc, clip->dist + i * bytes);
        ok = vmafx_submit(context, ref, dist, i, NULL) == VMAFX_OK;
    }
    return ok && vmafx_flush(context, NULL) == VMAFX_OK;
}

static bool same_scores(VmafxContext *a, const VmafxModel *ma, VmafxContext *b,
                        const VmafxModel *mb, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        VmafxScore sa = VMAFX_SCORE_INIT;
        VmafxScore sb = VMAFX_SCORE_INIT;
        if (vmafx_score_frame(a, ma, i, &sa, NULL) != VMAFX_OK ||
            vmafx_score_frame(b, mb, i, &sb, NULL) != VMAFX_OK ||
            memcmp(&sa.value, &sb.value, sizeof(sa.value)) != 0) {
            return false;
        }
    }
    return true;
}

/* Submit every frame of `clip` to both contexts: two references, no copy. */
static bool score_shared(VmafxContext *a, VmafxContext *b, Clip *clip, unsigned n)
{
    bool ok = true;
    for (unsigned i = 0; i < n && ok; i++) {
        VmafxFrame *ref = clip_frame(clip, true, i);
        VmafxFrame *dist = clip_frame(clip, false, i);
        VmafxFrame *ref_b = vmafx_frame_ref(ref);
        VmafxFrame *dist_b = vmafx_frame_ref(dist);
        ok = vmafx_submit(a, ref, dist, i, NULL) == VMAFX_OK &&
             vmafx_submit(b, ref_b, dist_b, i, NULL) == VMAFX_OK;
    }
    return ok && vmafx_flush(a, NULL) == VMAFX_OK && vmafx_flush(b, NULL) == VMAFX_OK;
}

static char *test_one_frame_two_contexts(void)
{
    Clip clip;
    mu_assert("clip", clip_open(&clip, 8));
    VmafxModel *ma = NULL;
    VmafxModel *mb = NULL;
    VmafxContext *a = vmaf_context("3.0", &ma);
    VmafxContext *b = vmaf_context("6.0", &mb);
    mu_assert("contexts", a && b && score_shared(a, b, &clip, N_FRAMES));
    mu_assert("no frame released while a context reads it", clip.released < 2u * N_FRAMES);
    VmafxModel *ma_alone = NULL;
    VmafxModel *mb_alone = NULL;
    VmafxContext *a_alone = vmaf_context("3.0", &ma_alone);
    VmafxContext *b_alone = vmaf_context("6.0", &mb_alone);
    mu_assert("alone",
              score_alone(a_alone, &clip, N_FRAMES) && score_alone(b_alone, &clip, N_FRAMES));
    mu_assert("A equals its separate run", same_scores(a, ma, a_alone, ma_alone, N_FRAMES));
    mu_assert("B equals its separate run", same_scores(b, mb, b_alone, mb_alone, N_FRAMES));
    mu_assert("the options differ in effect", !same_scores(a, ma, b, mb, N_FRAMES));
    mu_assert("destroy", vmafx_context_destroy(a, NULL) == VMAFX_OK &&
                             vmafx_context_destroy(b, NULL) == VMAFX_OK &&
                             vmafx_context_destroy(a_alone, NULL) == VMAFX_OK &&
                             vmafx_context_destroy(b_alone, NULL) == VMAFX_OK);
    vmafx_model_unref(ma);
    vmafx_model_unref(mb);
    vmafx_model_unref(ma_alone);
    vmafx_model_unref(mb_alone);
    mu_assert("each frame released exactly once, after both", clip.released == 2u * N_FRAMES);
    clip_close(&clip);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_device_skeleton),        MU_TEST(test_create_host_geometry),
        MU_TEST(test_create_host_failures),   MU_TEST(test_wrap_host),
        MU_TEST(test_wrap_host_failures),     MU_TEST(test_frame_planes_failures),
        MU_TEST(test_submit_and_flush),       MU_TEST(test_submit_failures_consume_frames),
        MU_TEST(test_submit_geometry_named),  MU_TEST(test_retention_keeps_n_minus_2),
        MU_TEST(test_one_frame_two_contexts),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
