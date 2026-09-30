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
 * Since ADR-1408 the upload of a frame is shared: a context uploads each
 * plane once and every extractor reads that copy
 * (core/src/hip/shared_frame.h). The wait moved with it, so checks 3 and 4
 * repeat 1 and 2 with every extractor in one frame, which is where the
 * planes are shared.
 *
 * Four checks, over every HIP extractor that uploads from VmafPicture::data:
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
 *      float_motion_hip); this check can. Without a context the extractor
 *      uploads into planes of its own, so this is the unshared path.
 *
 *   3. test_shared_pooled_parity: check 1 with every extractor registered in
 *      one context, with and without frame subsampling (the temporal
 *      extractors then run on frames the others skip, and an upload must not
 *      overwrite a plane a skipped extractor's kernels still read). Each
 *      plane of a frame is uploaded at most once.
 *
 *   4. test_shared_recycle_after_frame: check 2 on the shared path. Every
 *      extractor submits the frame between vmaf_hip_shared_frame_begin() and
 *      vmaf_hip_shared_frame_end(), exactly as vmaf_read_pictures() drives
 *      them, and both pictures are overwritten the moment the frame ended.
 *
 * Skip behaviour: no HIP device, or a build without device kernels
 * (-ENOSYS from init), prints "[skip: ...]" and exits as skipped.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "hip/shared_frame.h"
#include "hip_pooled_fixture.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"
#include "libvmaf_priv.h"

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

/* float_ssim is pinned to scale 1 on both sides, so that the twin's SSIM
 * passes read the uploaded planes directly, whatever the fixture size. */
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
    /* Stages both pictures into pinned buffers in submit() (ADR-1390). */
    {"ssimulacra2_hip", "ssimulacra2", NULL, NULL, 1e-9, {"ssimulacra2"}},
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

/* ------------------------------------------------------------------ */
/* Every extractor in one frame: the shared planes (ADR-1408)           */
/* ------------------------------------------------------------------ */

/* Planes of a 4:2:0 frame pair: Y, U and V of both pictures. */
#define FRAME_PLANES 6u

/* scores[c][k * N_FRAMES + f] and have[...] for every case of one run. */
typedef struct AllScores {
    double value[N_CASES][MAX_KEYS * N_FRAMES];
    bool have[N_CASES][MAX_KEYS * N_FRAMES];
} AllScores;

/* Register every case's extractor (`hip` or its CPU twin), feed the pooled
 * frames and read back every score that was written. */
static int run_pooled_all(VmafContext *vmaf, bool hip, AllScores *out)
{
    int err = 0;
    for (size_t i = 0u; i < N_CASES && !err; i++) {
        const RaceCase *c = &race_cases[i];
        VmafFeatureDictionary *opts = NULL;
        err = case_opts(c, &opts);
        if (!err)
            err = vmaf_use_feature(vmaf, hip ? c->hip : c->cpu, opts);
    }
    if (!err)
        err = hip_fixture_feed_frames(vmaf);
    if (!err)
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    for (size_t i = 0u; i < N_CASES && !err; i++) {
        const RaceCase *c = &race_cases[i];
        for (unsigned k = 0u; k < MAX_KEYS && c->keys[k] != NULL; k++) {
            for (unsigned f = 0u; f < N_FRAMES; f++) {
                const unsigned at = (k * N_FRAMES) + f;
                out->have[i][at] =
                    vmaf_feature_score_at_index(vmaf, c->keys[k], &out->value[i][at], f) == 0;
            }
        }
    }
    return err;
}

/* One context with every extractor; *uploads is what the shared frame
 * uploaded over the run (HIP only). */
static int run_all(bool hip, unsigned n_subsample, AllScores *out, uint64_t *uploads)
{
    VmafHipState *hip_state = NULL;
    if (hip) {
        VmafHipConfiguration hip_cfg = {.device_index = -1};
        if (vmaf_hip_state_init(&hip_state, hip_cfg) != 0 || hip_state == NULL)
            return -ENODEV;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .n_subsample = n_subsample};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (!err && hip)
        err = vmaf_hip_import_state(vmaf, hip_state);
    if (!err)
        err = run_pooled_all(vmaf, hip, out);
    if (hip && vmaf != NULL)
        *uploads = vmaf_context_hip_plane_uploads_for_test(vmaf);
    const int close_err = (vmaf != NULL) ? vmaf_close(vmaf) : 0;
    if (hip)
        vmaf_hip_state_free(&hip_state);
    return err ? err : close_err;
}

/* Number of cases with a score missing on one side or beyond the tolerance. */
static unsigned count_off_cases(const AllScores *cpu, const AllScores *gpu)
{
    unsigned off = 0u;
    for (size_t i = 0u; i < N_CASES; i++) {
        const RaceCase *c = &race_cases[i];
        bool bad = false;
        for (unsigned at = 0u; at < MAX_KEYS * N_FRAMES; at++) {
            if (c->keys[at / N_FRAMES] == NULL)
                break;
            const bool same_frames = cpu->have[i][at] == gpu->have[i][at];
            const double delta = fabs(cpu->value[i][at] - gpu->value[i][at]);
            if (!same_frames || (cpu->have[i][at] && delta > c->tol)) {
                (void)fprintf(stderr, "\n%s %s frame %u: cpu=%.17g hip=%.17g", c->hip,
                              c->keys[at / N_FRAMES], at % N_FRAMES, cpu->value[i][at],
                              gpu->value[i][at]);
                bad = true;
            }
        }
        off += bad ? 1u : 0u;
    }
    return off;
}

static char *check_shared_pooled(unsigned n_subsample)
{
    static AllScores cpu;
    static AllScores gpu;
    (void)memset(&cpu, 0, sizeof(cpu));
    (void)memset(&gpu, 0, sizeof(gpu));
    uint64_t uploads = 0u;
    mu_assert("CPU extraction of every twin failed", run_all(false, n_subsample, &cpu, NULL) == 0);
    mu_assert("HIP extraction of every twin in one context failed",
              run_all(true, n_subsample, &gpu, &uploads) == 0);
    const unsigned off = count_off_cases(&cpu, &gpu);
    (void)fprintf(stderr,
                  "[subsample %u: %zu extractors, %u off, %llu plane uploads in %u frames] ",
                  n_subsample, N_CASES, off, (unsigned long long)uploads, N_FRAMES);
    mu_assert("an extractor scored a shared frame against other samples than its own", off == 0u);
    mu_assert("the extractors of a context did not share the frame's planes", uploads > 0u);
    mu_assert("a plane of a frame was uploaded more than once",
              uploads <= (uint64_t)FRAME_PLANES * N_FRAMES);
    if (n_subsample <= 1u) {
        mu_assert("every plane of every frame is uploaded exactly once",
                  uploads == (uint64_t)FRAME_PLANES * N_FRAMES);
    }
    return NULL;
}

static char *test_shared_pooled_parity(void)
{
    if (!probe_hip())
        return NULL;
    char *msg = check_shared_pooled(1u);
    if (msg == NULL)
        msg = check_shared_pooled(2u);
    return msg;
}

/* Every case's extractor, driven through the extractor API on one shared
 * frame. */
typedef struct DirectRun {
    VmafHipSharedFrame *frame;
    VmafFeatureExtractorContext *ctx[N_CASES];
    VmafPicture ref;
    VmafPicture dist;
} DirectRun;

static int direct_open(DirectRun *run)
{
    int err = vmaf_hip_shared_frame_create(&run->frame);
    for (size_t i = 0u; i < N_CASES && !err; i++) {
        const RaceCase *c = &race_cases[i];
        VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(c->hip);
        VmafDictionary *opts = NULL;
        err = (fex == NULL) ? -EINVAL : 0;
        if (!err && c->opt_key != NULL)
            err = vmaf_dictionary_set(&opts, c->opt_key, c->opt_val, 0);
        if (!err)
            err = vmaf_feature_extractor_context_create(&run->ctx[i], fex, opts);
        if (err && opts != NULL)
            (void)vmaf_dictionary_free(&opts);
        if (!err) {
            /* What fex_ctx_bind_backends() does for a registered extractor. */
            run->ctx[i]->fex->hip_frame = run->frame;
            err = vmaf_feature_extractor_context_init(run->ctx[i], VMAF_PIX_FMT_YUV420P,
                                                      FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
        }
    }
    if (!err) {
        err =
            vmaf_picture_alloc(&run->ref, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    }
    if (!err) {
        err =
            vmaf_picture_alloc(&run->dist, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    }
    return err;
}

static void direct_close(DirectRun *run)
{
    for (size_t i = 0u; i < N_CASES; i++) {
        if (run->ctx[i] == NULL)
            continue;
        (void)vmaf_feature_extractor_context_close(run->ctx[i]);
        (void)vmaf_feature_extractor_context_destroy(run->ctx[i]);
    }
    if (run->ref.ref != NULL)
        (void)vmaf_picture_unref(&run->ref);
    if (run->dist.ref != NULL)
        (void)vmaf_picture_unref(&run->dist);
    /* After the extractors: they hold planes of it until they are closed. */
    vmaf_hip_shared_frame_destroy(&run->frame);
}

/* One frame as vmaf_read_pictures() drives it: every extractor submits
 * between begin() and end(). With `recycle`, both pictures are overwritten
 * with the next frame as soon as the frame ended. */
static int direct_frame(DirectRun *run, unsigned f, int recycle, VmafFeatureCollector *fc)
{
    hip_fixture_fill(&run->ref, f, 0u);
    hip_fixture_fill(&run->dist, f, 1u);
    int err = vmaf_hip_shared_frame_begin(run->frame, &run->ref, &run->dist);
    for (size_t i = 0u; i < N_CASES && !err; i++) {
        err = vmaf_feature_extractor_context_submit(run->ctx[i], &run->ref, NULL, &run->dist, NULL,
                                                    f);
    }
    vmaf_hip_shared_frame_end(run->frame);
    if (!err && recycle) {
        hip_fixture_fill(&run->ref, f + 1u, 0u);
        hip_fixture_fill(&run->dist, f + 1u, 1u);
    }
    for (size_t i = 0u; i < N_CASES && !err; i++)
        err = vmaf_feature_extractor_context_collect(run->ctx[i], f, fc);
    return err;
}

static int run_direct_shared(int recycle, VmafFeatureCollector *fc, uint64_t *uploads)
{
    DirectRun run = {0};
    int err = direct_open(&run);
    for (unsigned f = 0u; f < N_FRAMES && !err; f++)
        err = direct_frame(&run, f, recycle, fc);
    for (size_t i = 0u; i < N_CASES && !err; i++)
        (void)vmaf_feature_extractor_context_flush(run.ctx[i], fc);
    *uploads = vmaf_hip_shared_frame_upload_count(run.frame);
    direct_close(&run);
    return err;
}

static char *test_shared_recycle_after_frame(void)
{
    if (!probe_hip())
        return NULL;
    VmafFeatureCollector *clean = NULL;
    VmafFeatureCollector *recycled = NULL;
    mu_assert("feature collector init failed", vmaf_feature_collector_init(&clean) == 0);
    mu_assert("feature collector init failed", vmaf_feature_collector_init(&recycled) == 0);
    uint64_t uploads_clean = 0u;
    uint64_t uploads_recycled = 0u;
    const int err_clean = run_direct_shared(0, clean, &uploads_clean);
    const int err_recycled = run_direct_shared(1, recycled, &uploads_recycled);
    unsigned compared = 0u;
    const unsigned differing = count_differing(clean, recycled, &compared);
    vmaf_feature_collector_destroy(clean);
    vmaf_feature_collector_destroy(recycled);
    (void)fprintf(stderr,
                  "[%zu extractors on one shared frame, %u scores, %u changed, %llu uploads] ",
                  N_CASES, compared, differing, (unsigned long long)uploads_recycled);
    mu_assert("HIP extraction on a shared frame failed", !err_clean && !err_recycled);
    mu_assert("the extractors emitted no score to compare", compared > 0u);
    mu_assert("the extractors did not share the frame's planes",
              uploads_clean == (uint64_t)FRAME_PLANES * N_FRAMES &&
                  uploads_recycled == uploads_clean);
    mu_assert("a shared plane was still being uploaded when the frame ended", differing == 0u);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_pooled_parity);
    mu_run_test(test_recycle_after_submit);
    mu_run_test(test_shared_pooled_parity);
    mu_run_test(test_shared_recycle_after_frame);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
