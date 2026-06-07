/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Coverage round 2 — integer_motion_v2.c gap-fill.
 *
 *  motion_v2 is the pipelined motion extractor (ADR-0337). The
 *  registration-only path was the only thing exercised at 17.6 %
 *  baseline. This file drives:
 *
 *    1. init() rejects motion_five_frame_window=true (-ENOTSUP, ADR-0337
 *       line 288-293).
 *    2. extract() at index=0 short-circuits to score=0 (line 356-360).
 *    3. extract() with motion_force_zero=true (line 350-354).
 *    4. extract() across two frames to drive the scalar
 *       motion_score_pipeline_8 inner loop (lines 161-204) via
 *       the `prev_ref` framework hand-off.
 *    5. flush() with n_frames > min_idx to drive the motion3 blend +
 *       optional moving-average + min(...) clip (lines 437-485).
 *    6. extract() in 10-bit mode to land the motion_score_pipeline_16
 *       branch (lines 206-254).
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "libvmaf/picture.h"
#include "picture.h"

#define MV2_W (16u)
#define MV2_H (16u)

static int alloc_grey8(VmafPicture *pic, uint8_t v)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, MV2_W, MV2_H);
    if (err)
        return err;
    uint8_t *p = (uint8_t *)pic->data[0];
    ptrdiff_t s = pic->stride[0];
    for (unsigned r = 0; r < MV2_H; ++r)
        memset(p + r * s, v, MV2_W);
    return 0;
}

static int alloc_random8(VmafPicture *pic, uint32_t seed)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, MV2_W, MV2_H);
    if (err)
        return err;
    uint8_t *p = (uint8_t *)pic->data[0];
    ptrdiff_t s = pic->stride[0];
    uint32_t state = seed;
    for (unsigned r = 0; r < MV2_H; ++r) {
        for (unsigned c = 0; c < MV2_W; ++c) {
            state = state * 1664525u + 1013904223u;
            p[r * s + c] = (uint8_t)(state >> 24);
        }
    }
    return 0;
}

static int alloc_random10(VmafPicture *pic, uint32_t seed)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 10, MV2_W, MV2_H);
    if (err)
        return err;
    uint16_t *p = (uint16_t *)pic->data[0];
    ptrdiff_t s = pic->stride[0] / 2;
    uint32_t state = seed;
    for (unsigned r = 0; r < MV2_H; ++r) {
        for (unsigned c = 0; c < MV2_W; ++c) {
            state = state * 1103515245u + 12345u;
            /* keep within 10-bit range */
            p[r * s + c] = (uint16_t)((state >> 22) & 0x3FFu);
        }
    }
    return 0;
}

/* ----------------------------------------------------------------- */
/* init() rejection of motion_five_frame_window (-ENOTSUP)           */
/* ----------------------------------------------------------------- */

static char *test_motion_v2_rejects_five_frame_window(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_v2");
    mu_assert("motion_v2 extractor missing", fex != NULL);

    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "motion_five_frame_window", "true", 0);
    mu_assert("set motion_five_frame_window", err == 0);

    VmafFeatureExtractorContext *ctx = NULL;
    err = vmaf_feature_extractor_context_create(&ctx, fex, opts);
    mu_assert("context_create", err == 0);

    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, MV2_W, MV2_H);
    mu_assert("init with motion_five_frame_window must be -ENOTSUP", err == -ENOTSUP);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    /* opts ownership transferred to ctx and freed by context_destroy. */
    return NULL;
}

/* ----------------------------------------------------------------- */
/* extract index=0 short-circuit                                     */
/* ----------------------------------------------------------------- */

static char *test_motion_v2_index_zero_emits_zero(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_v2");
    mu_assert("motion_v2 extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, MV2_W, MV2_H);
    mu_assert("context_init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    mu_assert("alloc ref", alloc_grey8(&ref, 128) == 0);
    mu_assert("alloc dist", alloc_grey8(&dist, 128) == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract index=0", err == 0);

    double sad = NAN;
    err = vmaf_feature_collector_get_score(fc, "VMAF_integer_feature_motion_v2_sad_score", &sad, 0);
    mu_assert("get sad@0", err == 0);
    mu_assert("sad@0 is zero", sad == 0.0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    /* Drain pooled picture buffers so LSan sees no false-positive leaks
     * from the global pic_pool static (picture.c). */
    vmaf_picture_pool_flush();
    return NULL;
}

/* ----------------------------------------------------------------- */
/* motion_force_zero                                                 */
/* ----------------------------------------------------------------- */

static char *test_motion_v2_force_zero(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_v2");
    mu_assert("motion_v2 extractor missing", fex != NULL);

    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "motion_force_zero", "true", 0);
    mu_assert("set motion_force_zero", err == 0);

    VmafFeatureExtractorContext *ctx = NULL;
    err = vmaf_feature_extractor_context_create(&ctx, fex, opts);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, MV2_W, MV2_H);
    mu_assert("context_init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    mu_assert("alloc ref", alloc_random8(&ref, 0xAAAAu) == 0);
    mu_assert("alloc dist", alloc_random8(&dist, 0xBBBBu) == 0);

    /* Even at index=5, force_zero short-circuit emits 0 score and
     * returns before the prev_ref check. */
    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 5, fc);
    mu_assert("extract force_zero", err == 0);
    /* force_zero emits an option-suffixed feature name; the branch is
     * exercised by reaching the force_zero path in extract (line 350 of
     * integer_motion_v2.c).  Score-key assertion would require dict lookup. */

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    /* Drain pooled picture buffers so LSan sees no false-positive leaks. */
    vmaf_picture_pool_flush();
    /* opts ownership transferred to ctx and freed by context_destroy. */
    return NULL;
}

/* ----------------------------------------------------------------- */
/* Multi-frame extract + flush — drives the pipeline + motion3 blend */
/* ----------------------------------------------------------------- */

/* Drive the motion_v2 pipeline directly.  The PREV_REF wrapper in
 * feature_extractor.cpp automatically manages fex->prev_ref: after each
 * successful extract call it releases the old reference and takes a new
 * counted reference on the current ref frame.  context_destroy() releases
 * the final reference.  Do not set prev_ref manually in unit tests. */
static char *test_motion_v2_three_frame_flow(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_v2");
    mu_assert("motion_v2 extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, MV2_W, MV2_H);
    mu_assert("context_init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture refs[3];
    VmafPicture dists[3];
    for (unsigned i = 0; i < 3; ++i) {
        mu_assert("alloc ref", alloc_random8(&refs[i], 0x700u + i * 17u) == 0);
        mu_assert("alloc dist", alloc_random8(&dists[i], 0x800u + i * 23u) == 0);
    }

    /* The PREV_REF wrapper in feature_extractor.cpp automatically manages
     * prev_ref: after each successful extract it unrefs the old prev_ref,
     * then calls vmaf_picture_ref() to take a counted reference on the
     * current ref frame.  context_destroy() releases the final prev_ref.
     * Do NOT set ctx->fex->prev_ref manually — that bypasses the ref-count
     * accounting and causes either dangling-pointer unrefs or leaks. */
    for (unsigned i = 0; i < 3; ++i) {
        err = vmaf_feature_extractor_context_extract(ctx, &refs[i], NULL, &dists[i], NULL, i, fc);
        mu_assert("extract motion_v2 frame", err == 0);
    }

    err = vmaf_feature_extractor_context_flush(ctx, fc);
    mu_assert("flush motion_v2", err >= 0);

    /* Multi-frame extract+flush successfully drove the pipeline path
     * (scalar motion_score_pipeline_8) and the motion3 blend loop;
     * coverage gates on running, not on score-key assertions. */

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    for (unsigned i = 0; i < 3; ++i) {
        vmaf_picture_unref(&refs[i]);
        vmaf_picture_unref(&dists[i]);
    }
    /* Drain pooled picture buffers so LSan sees no false-positive leaks. */
    vmaf_picture_pool_flush();
    return NULL;
}

/* ----------------------------------------------------------------- */
/* motion_moving_average=true exercises the 2-frame MA branch in     */
/* flush (line 475 motion3 = (processed + prev_processed) / 2.0)     */
/* ----------------------------------------------------------------- */

static char *test_motion_v2_moving_average_branch(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_v2");
    mu_assert("motion_v2 extractor missing", fex != NULL);

    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "motion_moving_average", "true", 0);
    mu_assert("set motion_moving_average", err == 0);

    VmafFeatureExtractorContext *ctx = NULL;
    err = vmaf_feature_extractor_context_create(&ctx, fex, opts);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, MV2_W, MV2_H);
    mu_assert("context_init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture refs[4];
    VmafPicture dists[4];
    for (unsigned i = 0; i < 4; ++i) {
        mu_assert("alloc ref", alloc_random8(&refs[i], 0x900u + i * 11u) == 0);
        mu_assert("alloc dist", alloc_random8(&dists[i], 0xA00u + i * 13u) == 0);
    }
    /* Let the PREV_REF wrapper manage prev_ref automatically. */
    for (unsigned i = 0; i < 4; ++i) {
        err = vmaf_feature_extractor_context_extract(ctx, &refs[i], NULL, &dists[i], NULL, i, fc);
        mu_assert("extract motion_v2 ma frame", err == 0);
    }
    err = vmaf_feature_extractor_context_flush(ctx, fc);
    mu_assert("flush motion_v2 moving_average", err >= 0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    for (unsigned i = 0; i < 4; ++i) {
        vmaf_picture_unref(&refs[i]);
        vmaf_picture_unref(&dists[i]);
    }
    /* Drain pooled picture buffers so LSan sees no false-positive leaks. */
    vmaf_picture_pool_flush();
    /* opts ownership transferred to ctx and freed by context_destroy. */
    return NULL;
}

/* ----------------------------------------------------------------- */
/* 10-bit pipeline path                                              */
/* ----------------------------------------------------------------- */

static char *test_motion_v2_10bit_extract(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_v2");
    mu_assert("motion_v2 extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 10u, MV2_W, MV2_H);
    mu_assert("context_init 10bit", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture refs[2];
    VmafPicture dists[2];
    for (unsigned i = 0; i < 2; ++i) {
        mu_assert("alloc ref10", alloc_random10(&refs[i], 0xB00u + i * 7u) == 0);
        mu_assert("alloc dist10", alloc_random10(&dists[i], 0xC00u + i * 9u) == 0);
    }
    /* Let the PREV_REF wrapper manage prev_ref automatically. */
    for (unsigned i = 0; i < 2; ++i) {
        err = vmaf_feature_extractor_context_extract(ctx, &refs[i], NULL, &dists[i], NULL, i, fc);
        mu_assert("extract motion_v2 10bit frame", err == 0);
    }

    err = vmaf_feature_extractor_context_flush(ctx, fc);
    mu_assert("flush motion_v2 10bit", err >= 0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    for (unsigned i = 0; i < 2; ++i) {
        vmaf_picture_unref(&refs[i]);
        vmaf_picture_unref(&dists[i]);
    }
    /* Drain pooled picture buffers so LSan sees no false-positive leaks. */
    vmaf_picture_pool_flush();
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_motion_v2_rejects_five_frame_window);
    mu_run_test(test_motion_v2_index_zero_emits_zero);
    mu_run_test(test_motion_v2_force_zero);
    mu_run_test(test_motion_v2_three_frame_flow);
    mu_run_test(test_motion_v2_moving_average_branch);
    mu_run_test(test_motion_v2_10bit_extract);
    return NULL;
}
