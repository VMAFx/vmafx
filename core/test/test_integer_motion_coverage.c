/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Coverage round 2 — integer_motion.c gap-fill.
 *
 *  Plugs uncovered branches in core/src/feature/integer_motion.c
 *  (line 71.8% baseline, 2026-05-31 coverage run):
 *
 *    1. motion_force_zero=true: forces extract_force_zero + close_force_zero
 *       paths (lines 307-343).
 *    2. extract() with debug=true & default 3-frame window across multiple
 *       frames (drives the second-SAD branch + previous-score accumulation,
 *       lines 510-577).
 *    3. flush() with motion_five_frame_window=true and index<2 — exercises
 *       the under-min-past-frames branch in flush (lines 441-455).
 *    4. motion_moving_average=true to flip the moving-average branch in
 *       flush (line 435-437).
 *
 *  The init() failure cleanup at lines 411-419 is hit transitively when
 *  picture allocations fail; we drive that by injecting a 1x1 frame
 *  past the min-dim gate (handled by feature_extractor.c invariants
 *  already; the pure-cleanup path is reached only via OOM, which is
 *  exercised by the test_cuda_buffer_alloc_oom counterpart at higher
 *  scope and is not portable to a CPU unit test).
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "picture.h" /* vmaf_picture_ref (internal, not in public header) */
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#define MOT_W (16u)
#define MOT_H (16u)

static int alloc_grey(VmafPicture *pic, uint8_t v)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, MOT_W, MOT_H);
    if (err)
        return err;
    uint8_t *p = (uint8_t *)pic->data[0];
    ptrdiff_t s = pic->stride[0];
    for (unsigned r = 0; r < MOT_H; ++r)
        memset(p + r * s, v, MOT_W);
    /* Chroma planes left untouched (motion uses luma only). */
    return 0;
}

static int alloc_random(VmafPicture *pic, uint32_t seed)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, MOT_W, MOT_H);
    if (err)
        return err;
    uint8_t *p = (uint8_t *)pic->data[0];
    ptrdiff_t s = pic->stride[0];
    uint32_t state = seed;
    for (unsigned r = 0; r < MOT_H; ++r) {
        for (unsigned c = 0; c < MOT_W; ++c) {
            state = state * 1664525u + 1013904223u;
            p[r * s + c] = (uint8_t)(state >> 24);
        }
    }
    return 0;
}

/* Create+init a context for the "motion" extractor over a MOT_W x MOT_H
 * frame (optionally carrying `opts`) and init a feature collector. Every
 * test below starts this way once `fex` is looked up; context_create /
 * context_init use the same messages everywhere in this file. */
static char *motion_fixture_open(VmafFeatureExtractor *fex, VmafFeatureExtractorContext **ctx,
                                 VmafFeatureCollector **fc, VmafDictionary *opts)
{
    int err = vmaf_feature_extractor_context_create(ctx, fex, opts);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(*ctx, VMAF_PIX_FMT_YUV420P, 8u, MOT_W, MOT_H);
    mu_assert("context_init", err == 0);

    err = vmaf_feature_collector_init(fc);
    mu_assert("collector_init", err == 0);
    return NULL;
}

/* Allocate `n` (ref, dist) pairs of pseudo-random 8-bit frames, striding the
 * PRNG seed by `ref_step` / `dist_step` per index so consecutive pairs
 * differ. The single-frame caller passes n=1 and step=0. */
static char *alloc_motion_pairs(VmafPicture *refs, VmafPicture *dists, unsigned n,
                                uint32_t ref_seed0, uint32_t ref_step, uint32_t dist_seed0,
                                uint32_t dist_step)
{
    for (unsigned i = 0; i < n; ++i) {
        mu_assert("alloc ref", alloc_random(&refs[i], ref_seed0 + i * ref_step) == 0);
        mu_assert("alloc dist", alloc_random(&dists[i], dist_seed0 + i * dist_step) == 0);
    }
    return NULL;
}

/* integer_motion has VMAF_FEATURE_EXTRACTOR_PREV_REF: in the normal
 * vmaf_read_pictures() pipeline, libvmaf.c sets fex->prev_ref before calling
 * extract() and clears it afterwards. When driving the extractor directly
 * (as this coverage file does), we must replicate that protocol or extract()
 * returns -EINVAL at index >= 1 because prev->ref is NULL. Pattern mirrors
 * read_pictures_dispatch_one() in libvmaf.c. Runs extract() over `n` frames
 * of `refs` / `dists`, asserting "extract frame" after each call. */
static char *motion_extract_with_prev_ref(VmafFeatureExtractorContext *ctx,
                                          VmafFeatureCollector *fc, VmafPicture *refs,
                                          VmafPicture *dists, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        if (i > 0)
            vmaf_picture_ref(&ctx->fex->prev_ref, &refs[i - 1]);
        int err =
            vmaf_feature_extractor_context_extract(ctx, &refs[i], NULL, &dists[i], NULL, i, fc);
        if (ctx->fex->prev_ref.ref) {
            (void)vmaf_picture_unref(&ctx->fex->prev_ref);
            memset(&ctx->fex->prev_ref, 0, sizeof(ctx->fex->prev_ref));
        }
        mu_assert("extract frame", err == 0);
    }
    return NULL;
}

/* Common teardown for every test in this file: close/destroy `ctx`, destroy
 * `fc`, and unref `n` (ref, dist) pairs. */
static void motion_fixture_close(VmafFeatureExtractorContext *ctx, VmafFeatureCollector *fc,
                                 VmafPicture *refs, VmafPicture *dists, unsigned n)
{
    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    for (unsigned i = 0; i < n; ++i) {
        vmaf_picture_unref(&refs[i]);
        vmaf_picture_unref(&dists[i]);
    }
}

/* ----------------------------------------------------------------- */
/* motion_force_zero path                                            */
/* ----------------------------------------------------------------- */

static char *test_motion_force_zero_extract_returns_zero(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion");
    mu_assert("motion extractor missing", fex != NULL);

    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "motion_force_zero", "true", 0);
    mu_assert("set motion_force_zero", err == 0);

    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    char *msg = motion_fixture_open(fex, &ctx, &fc, opts);
    if (msg)
        return msg;

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey(&ref, 100);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey(&dist, 100);
    mu_assert("alloc dist", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract force_zero", err == 0);
    /* force_zero emits an option-suffixed feature name ("motion2_..._motion_force_zero=true").
     * The branch is exercised by reaching extract_force_zero (line 320 of integer_motion.c);
     * we don't assert on the score key directly. */

    motion_fixture_close(ctx, fc, &ref, &dist, 1);
    /* opts ownership transferred to ctx and freed by context_destroy. */
    return NULL;
}

/* ----------------------------------------------------------------- */
/* Three-frame default flow                                          */
/* ----------------------------------------------------------------- */

static char *test_motion_three_frame_extract_emits_scores(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion");
    mu_assert("motion extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    char *msg = motion_fixture_open(fex, &ctx, &fc, NULL);
    if (msg)
        return msg;

    VmafPicture refs[4];
    VmafPicture dists[4];
    msg = alloc_motion_pairs(refs, dists, 4, 0x100u, 17u, 0x200u, 23u);
    if (msg)
        return msg;

    msg = motion_extract_with_prev_ref(ctx, fc, refs, dists, 4);
    if (msg)
        return msg;

    int err = vmaf_feature_extractor_context_flush(ctx, fc);
    mu_assert("flush ok", err >= 0);

    /* index 2 has a real SAD2 (second-SAD branch at line 552-578).
     * motion2_score is emitted during flush(), not during extract(), so
     * vmaf_feature_collector_get_score must be called after flush(). */
    double m2 = NAN;
    err = vmaf_feature_collector_get_score(fc, "VMAF_integer_feature_motion2_score", &m2, 2);
    mu_assert("get motion2 frame2", err == 0);
    mu_assert("motion2 finite", isfinite(m2));

    motion_fixture_close(ctx, fc, refs, dists, 4);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* motion_moving_average=true (3-frame mode)                         */
/* ----------------------------------------------------------------- */

static char *test_motion_moving_average_branch(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion");
    mu_assert("motion extractor missing", fex != NULL);

    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "motion_moving_average", "true", 0);
    mu_assert("set motion_moving_average", err == 0);

    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    char *msg = motion_fixture_open(fex, &ctx, &fc, opts);
    if (msg)
        return msg;

    VmafPicture refs[3];
    VmafPicture dists[3];
    msg = alloc_motion_pairs(refs, dists, 3, 0x300u, 11u, 0x400u, 13u);
    if (msg)
        return msg;

    /* Same VMAF_FEATURE_EXTRACTOR_PREV_REF protocol as
     * test_motion_three_frame_extract_emits_scores. */
    msg = motion_extract_with_prev_ref(ctx, fc, refs, dists, 3);
    if (msg)
        return msg;

    err = vmaf_feature_extractor_context_flush(ctx, fc);
    mu_assert("flush moving_average", err >= 0);

    motion_fixture_close(ctx, fc, refs, dists, 3);
    /* opts ownership transferred to ctx and freed by context_destroy. */
    return NULL;
}

/* ----------------------------------------------------------------- */
/* Five-frame window (motion v1 supports it; flush branch with N<2). */
/* ----------------------------------------------------------------- */

static char *test_motion_five_frame_under_min(void)
{
    /* ADR-0337: motion_five_frame_window=true is rejected at init() with
     * -ENOTSUP.  Verify that, then exercise the flush(n<=min_idx) short-
     * circuit path (lines 403-415 of integer_motion.c) by running a
     * default-mode extractor with a single frame and flushing immediately.
     * Default mode: min_idx=1, so flush with n=1 hits the n<=min_idx arm. */
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion");
    mu_assert("motion extractor missing", fex != NULL);

    /* Part A: five_frame_window rejected at init */
    {
        VmafDictionary *opts = NULL;
        int err = vmaf_dictionary_set(&opts, "motion_five_frame_window", "true", 0);
        mu_assert("set motion_five_frame_window", err == 0);

        VmafFeatureExtractorContext *ctx_rej = NULL;
        err = vmaf_feature_extractor_context_create(&ctx_rej, fex, opts);
        mu_assert("context_create (rej)", err == 0);
        err = vmaf_feature_extractor_context_init(ctx_rej, VMAF_PIX_FMT_YUV420P, 8u, MOT_W, MOT_H);
        mu_assert("init must return -ENOTSUP for five_frame_window", err == -ENOTSUP);
        (void)vmaf_feature_extractor_context_close(ctx_rej);
        (void)vmaf_feature_extractor_context_destroy(ctx_rej);
    }

    /* Part B: default mode, single frame → flush exercises n<=min_idx arm */
    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    char *msg = motion_fixture_open(fex, &ctx, &fc, NULL);
    if (msg)
        return msg;

    VmafPicture ref;
    VmafPicture dist;
    msg = alloc_motion_pairs(&ref, &dist, 1, 0x500u, 0, 0x600u, 0);
    if (msg)
        return msg;

    int err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract frame0", err == 0);
    err = vmaf_feature_extractor_context_flush(ctx, fc);
    mu_assert("flush single-frame default mode", err >= 0);

    motion_fixture_close(ctx, fc, &ref, &dist, 1);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_motion_force_zero_extract_returns_zero);
    mu_run_test(test_motion_three_frame_extract_emits_scores);
    mu_run_test(test_motion_moving_average_branch);
    mu_run_test(test_motion_five_frame_under_min);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
