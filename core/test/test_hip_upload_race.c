/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Regression test for T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18: a HIP extractor
 * must not return from submit() while an upload from a host picture is still
 * in flight.
 *
 * HIP pictures are pageable host memory. An extractor that copied one with a
 * bare hipMemcpy2DAsync and returned left the copy reading a buffer the caller
 * was free to refill, so some frames were scored against the next frame's
 * samples, a different set on every run. Every extractor now stages its
 * pictures through vmaf_hip_picture_upload(), which waits for the copies.
 *
 * Two checks, over every HIP extractor that uploads from VmafPicture::data:
 *
 *   1. test_pooled_parity: the path a user takes. N_FRAMES frames from a
 *      picture pool sized like the CLI's (hip_pooled_fixture.h) through
 *      vmaf_read_pictures(), every frame within the extractor's parity
 *      tolerance of its CPU twin. The pool is LIFO, so the distorted picture
 *      of frame N is the first buffer refilled for frame N + 1.
 *
 *   2. test_recycle_after_submit: the invariant itself. Through the extractor
 *      API, both pictures are overwritten with the next frame the moment
 *      submit() returns, and every score must be bit-identical to a run that
 *      leaves them alone. vmaf_read_pictures() holds the reference picture
 *      for one more frame (prev_ref), so check 1 cannot see an extractor
 *      that uploads the reference only (motion_hip, motion_v2_hip,
 *      float_motion_hip); this check can.
 *
 * Skip behaviour: no HIP device, or a build without device kernels
 * (-ENOSYS from init), prints "[skip: ...]" and exits as skipped.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "hip_pooled_fixture.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this test mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#define MAX_KEYS 4u

typedef struct RaceCase {
    const char *hip;     /* HIP extractor */
    const char *cpu;     /* its CPU twin */
    const char *opt_key; /* option both need on this fixture, or NULL */
    const char *opt_val;
    double tol;                 /* the extractor's own parity-test tolerance */
    const char *keys[MAX_KEYS]; /* features compared, NULL-terminated */
} RaceCase;

/* float_ssim_hip supports scale=1 only and rejects the auto-selected scale
 * on larger fixtures, so both twins are pinned to it. */
static const RaceCase race_cases[] = {
    {"ciede_hip", "ciede", NULL, NULL, 1e-4, {"ciede2000"}},
    {"float_adm_hip", "float_adm", NULL, NULL, 1e-4, {"VMAF_feature_adm2_score"}},
    {"float_moment_hip",
     "float_moment",
     NULL,
     NULL,
     1e-4,
     {"float_moment_ref1st", "float_moment_dis1st", "float_moment_ref2nd", "float_moment_dis2nd"}},
    {"float_motion_hip", "float_motion", NULL, NULL, 1e-4, {"VMAF_feature_motion2_score"}},
    {"float_psnr_hip", "float_psnr", NULL, NULL, 1e-4, {"float_psnr"}},
    {"float_ssim_hip", "float_ssim", "scale", "1", 1e-3, {"float_ssim"}},
    {"float_vif_hip",
     "float_vif",
     NULL,
     NULL,
     1e-4,
     {"VMAF_feature_vif_scale0_score", "VMAF_feature_vif_scale1_score",
      "VMAF_feature_vif_scale2_score", "VMAF_feature_vif_scale3_score"}},
    {"motion_hip", "motion", NULL, NULL, 1e-4, {"VMAF_integer_feature_motion2_score"}},
    {"motion_v2_hip", "motion_v2", NULL, NULL, 1e-4, {"VMAF_integer_feature_motion_v2_sad_score"}},
    {"psnr_hip", "psnr", NULL, NULL, 1e-4, {"psnr_y", "psnr_cb", "psnr_cr"}},
    {"vif_hip",
     "vif",
     NULL,
     NULL,
     1e-4,
     {"VMAF_integer_feature_vif_scale0_score", "VMAF_integer_feature_vif_scale1_score",
      "VMAF_integer_feature_vif_scale2_score", "VMAF_integer_feature_vif_scale3_score"}},
    {"integer_ssim_hip", "ssim", NULL, NULL, 1e-4, {"ssim"}},
};
#define N_CASES (sizeof(race_cases) / sizeof(race_cases[0]))

/* 1 = a HIP device and device kernels are present, 0 = skip, -1 = not probed. */
static int hip_available = -1;

static int case_opts(const RaceCase *c, VmafFeatureDictionary **opts)
{
    *opts = NULL;
    if (c->opt_key == NULL)
        return 0;
    return vmaf_feature_dictionary_set(opts, c->opt_key, c->opt_val);
}

/* Pooled run of `extractor`; scores[k * N_FRAMES + f] is key k of frame f. */
static int run_pooled(VmafContext *vmaf, const RaceCase *c, const char *extractor, double *scores)
{
    VmafFeatureDictionary *opts = NULL;
    int err = case_opts(c, &opts);
    if (!err)
        err = vmaf_use_feature(vmaf, extractor, opts);
    if (!err)
        err = hip_fixture_feed_frames(vmaf);
    if (!err)
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    for (unsigned k = 0u; k < MAX_KEYS && c->keys[k] != NULL && !err; k++) {
        for (unsigned f = 0u; f < N_FRAMES && !err; f++) {
            err = vmaf_feature_score_at_index(vmaf, c->keys[k], &scores[k * N_FRAMES + f], f);
        }
    }
    return err;
}

static int run_pooled_cpu(const RaceCase *c, double *scores)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (err)
        return err;
    err = run_pooled(vmaf, c, c->cpu, scores);
    const int close_err = vmaf_close(vmaf);
    return err ? err : close_err;
}

static int run_pooled_hip(const RaceCase *c, double *scores)
{
    VmafHipState *hip_state = NULL;
    VmafHipConfiguration hip_cfg = {.device_index = -1};
    int err = vmaf_hip_state_init(&hip_state, hip_cfg);
    if (err != 0 || hip_state == NULL)
        return -ENODEV;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    if (!err)
        err = vmaf_hip_import_state(vmaf, hip_state);
    if (!err)
        err = run_pooled(vmaf, c, c->hip, scores);
    const int close_err = (vmaf != NULL) ? vmaf_close(vmaf) : 0;
    vmaf_hip_state_free(&hip_state);
    return err ? err : close_err;
}

/* Worst |cpu - hip| over every key and frame of one case; logs each frame
 * beyond the tolerance. */
static double worst_delta(const RaceCase *c, const double *cpu, const double *gpu)
{
    double worst = 0.0;
    for (unsigned k = 0u; k < MAX_KEYS && c->keys[k] != NULL; k++) {
        for (unsigned f = 0u; f < N_FRAMES; f++) {
            const double delta = fabs(cpu[k * N_FRAMES + f] - gpu[k * N_FRAMES + f]);
            if (delta > c->tol) {
                (void)fprintf(stderr, "\n%s %s frame %u: cpu=%.17g hip=%.17g delta=%.3e", c->hip,
                              c->keys[k], f, cpu[k * N_FRAMES + f], gpu[k * N_FRAMES + f], delta);
            }
            worst = delta > worst ? delta : worst;
        }
    }
    return worst;
}

/* Sets hip_available on first use: a missing device (-ENODEV) or a scaffold
 * build (-ENOSYS) skips the whole test rather than fails it. */
static int probe_hip(void)
{
    if (hip_available >= 0)
        return hip_available;
    double scores[MAX_KEYS * N_FRAMES] = {0.0};
    const int err = run_pooled_hip(&race_cases[0], scores);
    hip_available = (err == -ENODEV || err == -ENOSYS) ? 0 : 1;
    if (!hip_available) {
        (void)fprintf(stderr, "[skip: %s] ",
                      err == -ENODEV ? "no HIP device" : "HIP extractors are scaffolds (-ENOSYS)");
        mu_skipped = 1;
    }
    return hip_available;
}

static char *test_pooled_parity(void)
{
    if (!probe_hip())
        return NULL;
    unsigned failed = 0u;
    for (size_t i = 0u; i < N_CASES; i++) {
        const RaceCase *c = &race_cases[i];
        double cpu[MAX_KEYS * N_FRAMES] = {0.0};
        double gpu[MAX_KEYS * N_FRAMES] = {0.0};
        mu_assert("CPU extraction failed", run_pooled_cpu(c, cpu) == 0);
        mu_assert("HIP extraction failed", run_pooled_hip(c, gpu) == 0);
        const double worst = worst_delta(c, cpu, gpu);
        if (worst > c->tol) {
            (void)fprintf(stderr, "\npooled parity FAIL %s: max delta %.3e > %g\n", c->hip, worst,
                          c->tol);
            failed++;
        }
    }
    (void)fprintf(stderr, "[%ux%u, %u pooled frames, %zu extractors, %u off] ", FIXTURE_W,
                  FIXTURE_H, N_FRAMES, N_CASES, failed);
    mu_assert("a HIP extractor scored a pooled frame against another frame's samples",
              failed == 0u);
    return NULL;
}

/* Drives `c->hip` through the extractor API. With `recycle`, both pictures
 * are overwritten with the next frame as soon as submit() returns. */
static int run_direct(const RaceCase *c, int recycle, VmafFeatureCollector *fc)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(c->hip);
    if (fex == NULL)
        return -EINVAL;
    VmafDictionary *opts = NULL;
    int err = (c->opt_key != NULL) ? vmaf_dictionary_set(&opts, c->opt_key, c->opt_val, 0) : 0;
    VmafFeatureExtractorContext *ctx = NULL;
    if (!err)
        err = vmaf_feature_extractor_context_create(&ctx, fex, opts);
    VmafPicture ref = {0};
    VmafPicture dist = {0};
    if (!err)
        err = vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (!err)
        err = vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (!err) {
        err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W,
                                                  FIXTURE_H);
    }
    for (unsigned f = 0u; f < N_FRAMES && !err; f++) {
        hip_fixture_fill(&ref, f, 0u);
        hip_fixture_fill(&dist, f, 1u);
        err = vmaf_feature_extractor_context_submit(ctx, &ref, NULL, &dist, NULL, f);
        if (!err && recycle) {
            hip_fixture_fill(&ref, f + 1u, 0u);
            hip_fixture_fill(&dist, f + 1u, 1u);
        }
        if (!err)
            err = vmaf_feature_extractor_context_collect(ctx, f, fc);
    }
    if (!err)
        (void)vmaf_feature_extractor_context_flush(ctx, fc);
    if (ctx != NULL) {
        /* The context owns `opts` from here on. */
        (void)vmaf_feature_extractor_context_close(ctx);
        (void)vmaf_feature_extractor_context_destroy(ctx);
    } else if (opts != NULL) {
        (void)vmaf_dictionary_free(&opts);
    }
    if (ref.ref != NULL)
        (void)vmaf_picture_unref(&ref);
    if (dist.ref != NULL)
        (void)vmaf_picture_unref(&dist);
    return err;
}

/* Number of scores in `clean` that `recycled` does not reproduce bit for
 * bit; *compared counts the scores looked at. */
static unsigned count_differing(VmafFeatureCollector *clean, VmafFeatureCollector *recycled,
                                unsigned *compared)
{
    unsigned differing = 0u;
    *compared = 0u;
    for (unsigned i = 0u; i < clean->cnt; i++) {
        const FeatureVector *fv = clean->feature_vector[i];
        for (unsigned j = 0u; j < fv->capacity; j++) {
            if (!fv->score[j].written)
                continue;
            double other = 0.0;
            (*compared)++;
            if (vmaf_feature_collector_get_score(recycled, fv->name, &other, j) != 0 ||
                other != fv->score[j].value) {
                differing++;
            }
        }
    }
    return differing;
}

static char *check_recycle_case(const RaceCase *c, unsigned *failed)
{
    VmafFeatureCollector *clean = NULL;
    VmafFeatureCollector *recycled = NULL;
    mu_assert("feature collector init failed", vmaf_feature_collector_init(&clean) == 0);
    mu_assert("feature collector init failed", vmaf_feature_collector_init(&recycled) == 0);
    const int err_clean = run_direct(c, 0, clean);
    const int err_recycled = run_direct(c, 1, recycled);
    unsigned compared = 0u;
    const unsigned differing = count_differing(clean, recycled, &compared);
    vmaf_feature_collector_destroy(clean);
    vmaf_feature_collector_destroy(recycled);
    mu_assert("HIP extraction through the extractor API failed", !err_clean && !err_recycled);
    mu_assert("the extractor emitted no score to compare", compared > 0u);
    if (differing != 0u) {
        (void)fprintf(stderr,
                      "\nrecycle FAIL %s: %u of %u scores changed when the pictures were "
                      "refilled right after submit()\n",
                      c->hip, differing, compared);
        (*failed)++;
    }
    return NULL;
}

static char *test_recycle_after_submit(void)
{
    if (!probe_hip())
        return NULL;
    unsigned failed = 0u;
    for (size_t i = 0u; i < N_CASES; i++) {
        char *msg = check_recycle_case(&race_cases[i], &failed);
        if (msg)
            return msg;
    }
    (void)fprintf(stderr, "[%ux%u, %u frames, %zu extractors, %u still reading after submit()] ",
                  FIXTURE_W, FIXTURE_H, N_FRAMES, N_CASES, failed);
    mu_assert("a HIP extractor returned from submit() with a picture upload in flight",
              failed == 0u);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_pooled_parity);
    mu_run_test(test_recycle_after_submit);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
