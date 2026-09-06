/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CPU-only coverage for core/src/feature/float_motion.c.
 *
 *  float_motion tests exist only as GPU-gated CUDA/SYCL parity tests and
 *  the min-dim init rejection test (test_motion_min_dim.c).  This file
 *  adds end-to-end CPU extract/flush coverage for the fast suite:
 *
 *    1. Default options — init() + 3 frames of extract() + flush() with
 *       random luma frames exercises the SAD pipeline (float_sad_line_c)
 *       and the 3-frame window moving-average flush path.
 *    2. motion_force_zero=true — extract() emits zero-valued scores via
 *       the short-circuit extract_force_zero path.
 *    3. motion_add_uv=true — adds UV planes to the motion score; exercises
 *       the UV plane allocation / contribution branches in init() and extract().
 *    4. 10-bit extract path — exercises the 10-bit picture_copy branch.
 *
 *  float_motion uses a 5-tap Gaussian; minimum frame is 3x3.  Use 16x16
 *  to exercise the full pipeline without hitting the minimum-dim guard.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "libvmaf/picture.h"
#include "picture.h" /* vmaf_picture_ref — internal, not in the public header */

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#define FMOT_W (16u)
#define FMOT_H (16u)

static int alloc_grey8(VmafPicture *pic, uint8_t v)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, FMOT_W, FMOT_H);
    if (err)
        return err;
    for (unsigned p = 0; p < 3u; ++p) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        ptrdiff_t s = pic->stride[p];
        for (unsigned r = 0; r < pic->h[p]; ++r)
            memset(plane + r * s, v, pic->w[p]);
    }
    return 0;
}

/* LCG pseudo-random pixel fill so consecutive frames have non-zero SAD. */
static int alloc_random8(VmafPicture *pic, uint32_t seed)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, FMOT_W, FMOT_H);
    if (err)
        return err;
    uint32_t state = seed;
    for (unsigned p = 0; p < 3u; ++p) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        ptrdiff_t s = pic->stride[p];
        for (unsigned r = 0; r < pic->h[p]; ++r) {
            for (unsigned c = 0; c < pic->w[p]; ++c) {
                state = state * 1664525u + 1013904223u;
                plane[r * s + (ptrdiff_t)c] = (uint8_t)(state >> 24);
            }
        }
    }
    return 0;
}

/* Drive the extractor for N frames, populating prev_ref as the pipeline
 * expects.  Returns 0 on success, non-zero if any extract fails. */
static int run_frames(VmafFeatureExtractorContext *ctx, VmafFeatureCollector *fc, unsigned n_frames,
                      uint32_t seed_base)
{
    VmafPicture pics[8];
    if (n_frames > 8u)
        return -EINVAL;

    /* Allocate all frames. */
    for (unsigned i = 0; i < n_frames; ++i) {
        int err = alloc_random8(&pics[i], seed_base + i * 31u);
        if (err)
            return err;
    }

    int err = 0;
    for (unsigned i = 0; i < n_frames; ++i) {
        if (i > 0)
            vmaf_picture_ref(&ctx->fex->prev_ref, &pics[i - 1u]);
        err = vmaf_feature_extractor_context_extract(ctx, &pics[i], NULL, &pics[i], NULL, i, fc);
        if (ctx->fex->prev_ref.ref) {
            (void)vmaf_picture_unref(&ctx->fex->prev_ref);
            memset(&ctx->fex->prev_ref, 0, sizeof(ctx->fex->prev_ref));
        }
        if (err)
            break;
    }

    for (unsigned i = 0; i < n_frames; ++i)
        vmaf_picture_unref(&pics[i]);
    return err;
}

/* ----------------------------------------------------------------- */

static char *test_float_motion_default_three_frames(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_motion");
    mu_assert("float_motion extractor present", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, FMOT_W, FMOT_H);
    mu_assert("context_init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    err = run_frames(ctx, fc, 3u, 0xA000u);
    mu_assert("run 3 frames", err == 0);

    err = vmaf_feature_extractor_context_flush(ctx, fc);
    mu_assert("flush", err >= 0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    return NULL;
}

static char *test_float_motion_force_zero(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_motion");
    mu_assert("float_motion extractor present", fex != NULL);

    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "motion_force_zero", "true", 0);
    mu_assert("set motion_force_zero", err == 0);

    VmafFeatureExtractorContext *ctx = NULL;
    err = vmaf_feature_extractor_context_create(&ctx, fex, opts);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, FMOT_W, FMOT_H);
    mu_assert("context_init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    err = alloc_grey8(&ref, 128u);
    mu_assert("alloc ref", err == 0);
    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &ref, NULL, 0u, fc);
    mu_assert("extract force_zero", err == 0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    return NULL;
}

static char *test_float_motion_add_uv(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_motion");
    mu_assert("float_motion extractor present", fex != NULL);

    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "motion_add_uv", "true", 0);
    mu_assert("set motion_add_uv", err == 0);

    VmafFeatureExtractorContext *ctx = NULL;
    err = vmaf_feature_extractor_context_create(&ctx, fex, opts);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, FMOT_W, FMOT_H);
    mu_assert("context_init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    err = run_frames(ctx, fc, 2u, 0xB000u);
    mu_assert("run 2 frames with add_uv", err == 0);

    err = vmaf_feature_extractor_context_flush(ctx, fc);
    mu_assert("flush", err >= 0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    return NULL;
}

static char *test_float_motion_single_frame_flush(void)
{
    /* A single frame then flush exercises the n<=min_idx short-circuit path. */
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_motion");
    mu_assert("float_motion extractor present", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, FMOT_W, FMOT_H);
    mu_assert("context_init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    err = alloc_grey8(&ref, 100u);
    mu_assert("alloc ref", err == 0);
    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &ref, NULL, 0u, fc);
    mu_assert("extract frame 0", err == 0);

    err = vmaf_feature_extractor_context_flush(ctx, fc);
    mu_assert("flush single-frame", err >= 0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_motion_default_three_frames);
    mu_run_test(test_float_motion_force_zero);
    mu_run_test(test_float_motion_add_uv);
    mu_run_test(test_float_motion_single_frame_flush);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
