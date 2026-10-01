/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-0883 round-2 — MS-SSIM CPU vs. HIP parity test.
 *
 * Multi-Scale SSIM is computed by float_ms_ssim.c (CPU) and by
 * integer_ms_ssim_hip.c + integer_ms_ssim/ms_ssim_score.hip (HIP).
 * The HIP path has no cross-backend assertion before this test; a
 * regression in the per-scale Gaussian pyramid + multi-scale product
 * would silently shift downstream metrics.
 *
 * Asserts the single emitted `float_ms_ssim` channel.  Tolerance is
 * places=3 (1e-3) — multi-scale reduction amplifies per-window
 * rounding more than single-scale SSIM, so we use the same budget
 * VIF gets per ADR-0214.
 *
 * Since ADR-1403 the twin is the CPU's arithmetic, and
 * test_ms_ssim_matches_cpu_bit_for_bit holds every output of every frame,
 * the 15 per-scale l / c / s means of `enable_lcs` included, to the CPU's
 * value bit for bit. The tolerance tests above it stay as the coarse gate
 * for the dB options.
 *
 * Skip behaviour: if vmaf_hip_state_init() fails (no HIP runtime or
 * no device visible) the test emits "[skip: no HIP device]" and passes.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"

/* MS-SSIM has a 5-level 11-tap Gaussian pyramid; each downscale halves the
 * dimensions.  The minimum admissible dimension is GAUSSIAN_LEN << (SCALES-1)
 * = 11 << 4 = 176 (float_ms_ssim.c:131).  256x192 satisfies both axes with
 * margin and keeps the smallest scale at 16x12 — above the 11x11 window.
 * A fixture shorter than 176 px returns -EINVAL from vmaf_read_pictures. */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 192u
#endif
#define FIXTURE_BPC 8u
#define PARITY_TOL 1e-3

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + salt * 19u) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            memset(plane + row * pic->stride[p], 128, pic->w[p]);
        }
    }
    return 0;
}

static int feed_frame(VmafContext *vmaf, bool identical)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_pic(&ref, 0u);
    if (err)
        return err;
    /* An identical pair drives ms_ssim to 1.0, which is where the ADR-1221
     * dB ceiling actually binds. */
    err = fill_pic(&dist, identical ? 0u : 1u);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, 0u);
}

/* ADR-1221 — `enable_db` / `clip_db` opt into the dB-domain score with a
 * geometry-derived ceiling. Neither is a VMAF_OPT_FLAG_FEATURE_PARAM, so the
 * collector key stays `float_ms_ssim`. */
static int ms_ssim_db_opts(VmafFeatureDictionary **opts)
{
    int err = vmaf_feature_dictionary_set(opts, "enable_db", "true");
    if (err)
        return err;
    return vmaf_feature_dictionary_set(opts, "clip_db", "true");
}

/* The device, or NULL with the reason printed when there is none. */
static VmafHipState *ms_hip_state(void)
{
    VmafHipState *hip_state = NULL;
    VmafHipConfiguration hip_cfg = {.device_index = -1};
    if (vmaf_hip_state_init(&hip_state, hip_cfg) != 0 || hip_state == NULL) {
        (void)fprintf(stderr, "[skip: no HIP device] ");
        return NULL;
    }
    return hip_state;
}

/* A context with one MS-SSIM extractor: `integer_ms_ssim_hip` on `hip_state`,
 * or the CPU's `float_ms_ssim` when it is NULL. `opts` is consumed. */
static int ms_context_new(VmafContext **vmaf, VmafHipState *hip_state, VmafFeatureDictionary *opts)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    int err = vmaf_init(vmaf, cfg);
    if (!err && hip_state)
        err = vmaf_hip_import_state(*vmaf, hip_state);
    if (!err) {
        err = vmaf_use_feature(*vmaf, hip_state ? "integer_ms_ssim_hip" : "float_ms_ssim", opts);
        opts = err ? opts : NULL; /* taken on success */
    }
    if (opts)
        (void)vmaf_feature_dictionary_free(&opts);
    return err;
}

/* `float_ms_ssim` of one frame pair from one extractor. */
static int ms_single_score(VmafHipState *hip_state, bool db, bool identical, double *score)
{
    VmafFeatureDictionary *opts = NULL;
    int err = db ? ms_ssim_db_opts(&opts) : 0;
    VmafContext *vmaf = NULL;
    if (err) {
        (void)vmaf_feature_dictionary_free(&opts);
        return err;
    }
    err = ms_context_new(&vmaf, hip_state, opts);
    if (!err)
        err = feed_frame(vmaf, identical);
    if (!err)
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (!err)
        err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim", score, 0u);
    const int closed = vmaf ? vmaf_close(vmaf) : 0;
    return err ? err : closed;
}

/* The CPU's and the twin's score of one frame pair. `*skipped` is set, and 0
 * returned, when there is no device or the twin is a scaffold: an
 * unimplemented HIP extractor returns -ENOSYS (see the HIP extractors under
 * core/src/feature/hip/), which is a not-built-yet signal, not a regression.
 * Any other error is returned. */
static int ms_pair_scores(bool db, bool identical, double *cpu, double *gpu, bool *skipped)
{
    *skipped = true;
    VmafHipState *hip_state = ms_hip_state();
    if (!hip_state)
        return 0;
    const int gpu_err = ms_single_score(hip_state, db, identical, gpu);
    vmaf_hip_state_free(&hip_state);
    if (gpu_err == -ENOSYS) {
        (void)fprintf(stderr, "[skip: integer_ms_ssim_hip is a scaffold (-ENOSYS)] ");
        return 0;
    }
    if (gpu_err)
        return gpu_err;
    *skipped = false;
    return ms_single_score(NULL, db, identical, cpu);
}

static char *test_ms_ssim_hip_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("integer_ms_ssim_hip");
    mu_assert("integer_ms_ssim_hip extractor must be registered", fex != NULL);
    mu_assert("integer_ms_ssim_hip name matches", !strcmp(fex->name, "integer_ms_ssim_hip"));
    return NULL;
}

static char *test_ms_ssim_cpu_hip_parity(void)
{
    double cpu = 0.0;
    double gpu = NAN;
    bool skipped = false;
    mu_assert("float_ms_ssim: the CPU or the HIP run failed",
              ms_pair_scores(false, false, &cpu, &gpu, &skipped) == 0);
    if (skipped)
        return NULL;
    const double delta = fabs(cpu - gpu);
    if (!(delta <= PARITY_TOL)) {
        (void)fprintf(stderr, "\nms_ssim parity FAIL: cpu=%.8f hip=%.8f delta=%.2e tol=%.2e\n", cpu,
                      gpu, delta, PARITY_TOL);
    }
    mu_assert("float_ms_ssim CPU vs. HIP delta exceeds places=3 tolerance (1e-3)",
              delta <= PARITY_TOL);
    return NULL;
}

/* ADR-1221 — clip_db is a CEILING on the dB output, not a clamp on the linear
 * score. float_ms_ssim.c derives `max_db = ceil(10*log10(peak*peak/mse))` with
 * `mse = 0.5/(w*h)` and returns `MIN(-10*log10(1 - score), max_db)`,
 * short-circuiting to `max_db` when score >= 1.0. This twin used to clamp the
 * LINEAR score into [0, 1] and then convert with no ceiling, which returns
 * +Inf on an identical reference/distorted pair — an ordinary thing to score.
 * The default-options test above cannot see it: with enable_db off, neither
 * path converts at all. An identical pair drives ms_ssim to 1.0, which is
 * where the ceiling binds. */
static char *test_ms_ssim_clip_db_ceiling(void)
{
    double cpu = 0.0;
    double gpu = NAN;
    bool skipped = false;
    mu_assert("float_ms_ssim enable_db+clip_db: the CPU or the HIP run failed",
              ms_pair_scores(true, true, &cpu, &gpu, &skipped) == 0);
    if (skipped)
        return NULL;

    mu_assert("CPU float_ms_ssim dB score is non-finite", isfinite(cpu));
    mu_assert("HIP float_ms_ssim dB score is non-finite -- clip_db must cap it at max_db",
              isfinite(gpu));

    const double delta = fabs(cpu - gpu);
    if (!(delta <= PARITY_TOL)) {
        (void)fprintf(stderr,
                      "\nfloat_ms_ssim enable_db+clip_db parity FAIL: cpu=%.8f hip=%.8f "
                      "delta=%.2e tol=%.2e\n",
                      cpu, gpu, delta, PARITY_TOL);
    }
    mu_assert("float_ms_ssim dB score drifts from the CPU reference", delta <= PARITY_TOL);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* ADR-1403 — float_ms_ssim_hip is the CPU's arithmetic, bit for bit.    */
/*                                                                     */
/* The kernels compute each sample through integer_ms_ssim/             */
/* ms_ssim_arith.h: one fused multiply-add per decimate tap             */
/* (ms_ssim_decimate.c), fp32 products summed as an exact fp32 pair     */
/* that stands for iqa_convolve()'s fp64 sum, and the fp32 denominators */
/* and quotient of ssim_accumulate_default_scalar(). The host rounds    */
/* each per-scale mean to fp32 and combines as ms_ssim.c does. Before   */
/* that the twin ran fp32 running sums, fp64 denominators and unrounded */
/* means and was 5e-8 to 3e-6 from the CPU on a gfx1036, on every frame */
/* of the Netflix pair, the 1080p checkerboards and BBB 3840x2160.      */
/* ------------------------------------------------------------------ */
#define MS_EXACT_KEYS 16u
#define MS_EXACT_FRAMES 3u

typedef struct MsExactScores {
    double v[MS_EXACT_FRAMES][MS_EXACT_KEYS];
} MsExactScores;

static const char *const ms_exact_keys[MS_EXACT_KEYS] = {
    "float_ms_ssim",          "float_ms_ssim_l_scale0", "float_ms_ssim_l_scale1",
    "float_ms_ssim_l_scale2", "float_ms_ssim_l_scale3", "float_ms_ssim_l_scale4",
    "float_ms_ssim_c_scale0", "float_ms_ssim_c_scale1", "float_ms_ssim_c_scale2",
    "float_ms_ssim_c_scale3", "float_ms_ssim_c_scale4", "float_ms_ssim_s_scale0",
    "float_ms_ssim_s_scale1", "float_ms_ssim_s_scale2", "float_ms_ssim_s_scale3",
    "float_ms_ssim_s_scale4",
};

/* A textured reference and a dimmed, blocked copy of it, so l, c and s all
 * leave 1 at every scale; fill_pic()'s ramp alone leaves most of them at 1. */
static int ms_exact_fill(VmafPicture *pic, unsigned frame, bool distorted)
{
    const int err = fill_pic(pic, frame);
    if (err) {
        return err;
    }
    uint32_t state = 0x9E3779B9u ^ (frame * 2654435761u);
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            state = (state * 1664525u) + 1013904223u;
            unsigned value = (((col * 5u) + (row * 3u) + (frame * 11u)) & 127u) + 64u;
            value += (state >> 8) & 31u;
            if (distorted) {
                value = value - (value / 9u) + ((((col / 8u) ^ (row / 8u)) & 1u) * 6u);
            }
            y[(row * pic->stride[0]) + col] = (uint8_t)value;
        }
    }
    return 0;
}

static int ms_exact_feed(VmafContext *vmaf)
{
    for (unsigned i = 0; i < MS_EXACT_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = ms_exact_fill(&ref, i, false);
        if (err) {
            return err;
        }
        err = ms_exact_fill(&dist, i, true);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            return err;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err) {
            return err;
        }
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

static int ms_exact_collect(VmafContext *vmaf, MsExactScores *out)
{
    for (unsigned k = 0; k < MS_EXACT_KEYS; k++) {
        for (unsigned i = 0; i < MS_EXACT_FRAMES; i++) {
            const int err = vmaf_feature_score_at_index(vmaf, ms_exact_keys[k], &out->v[i][k], i);
            if (err) {
                return err;
            }
        }
    }
    return 0;
}

/* Every frame's 16 outputs from one extractor; `hip_state` NULL runs the CPU. */
static int ms_exact_score(VmafHipState *hip_state, MsExactScores *out)
{
    VmafFeatureDictionary *opts = NULL;
    VmafContext *vmaf = NULL;
    int err = vmaf_feature_dictionary_set(&opts, "enable_lcs", "true");
    if (err) {
        (void)vmaf_feature_dictionary_free(&opts);
        return err;
    }
    err = ms_context_new(&vmaf, hip_state, opts);
    if (!err) {
        err = ms_exact_feed(vmaf);
    }
    if (!err) {
        err = ms_exact_collect(vmaf, out);
    }
    const int closed = vmaf ? vmaf_close(vmaf) : 0;
    return err ? err : closed;
}

static uint64_t ms_exact_bits(double v)
{
    uint64_t bits = 0u;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

/* The outputs that are not the CPU's bit for bit, each one reported. */
static unsigned ms_exact_mismatches(const MsExactScores *cpu, const MsExactScores *gpu)
{
    unsigned differing = 0u;
    for (unsigned i = 0; i < MS_EXACT_FRAMES; i++) {
        for (unsigned k = 0; k < MS_EXACT_KEYS; k++) {
            if (ms_exact_bits(cpu->v[i][k]) == ms_exact_bits(gpu->v[i][k])) {
                continue;
            }
            differing++;
            (void)fprintf(stderr, "\n%s frame %u: cpu=%.17g hip=%.17g delta=%.3e", ms_exact_keys[k],
                          i, cpu->v[i][k], gpu->v[i][k], fabs(cpu->v[i][k] - gpu->v[i][k]));
        }
    }
    return differing;
}

static char *test_ms_ssim_matches_cpu_bit_for_bit(void)
{
    VmafHipState *hip_state = ms_hip_state();
    if (!hip_state)
        return NULL;
    MsExactScores cpu;
    MsExactScores gpu;
    memset(&cpu, 0, sizeof(cpu));
    memset(&gpu, 0, sizeof(gpu));
    const int gpu_err = ms_exact_score(hip_state, &gpu);
    vmaf_hip_state_free(&hip_state);
    if (gpu_err == -ENOSYS) {
        (void)fprintf(stderr, "[skip: integer_ms_ssim_hip is a scaffold (-ENOSYS)] ");
        return NULL;
    }
    mu_assert("HIP: integer_ms_ssim_hip with enable_lcs failed", gpu_err == 0);
    mu_assert("CPU: float_ms_ssim with enable_lcs failed", ms_exact_score(NULL, &cpu) == 0);
    mu_assert("CPU float_ms_ssim is not a usable reference",
              isfinite(cpu.v[1][0]) && cpu.v[1][0] > 0.0 && cpu.v[1][0] < 1.0);
    mu_assert("float_ms_ssim_hip is not the CPU extractor's value bit for bit",
              ms_exact_mismatches(&cpu, &gpu) == 0u);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_ms_ssim_hip_registered);
    mu_run_test(test_ms_ssim_cpu_hip_parity);
    mu_run_test(test_ms_ssim_clip_db_ceiling);
    mu_run_test(test_ms_ssim_matches_cpu_bit_for_bit);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
