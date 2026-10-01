/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-0947 — float_ms_ssim CPU vs. CUDA parity test (round 3).
 *
 * The float-path multi-scale SSIM extractor is implemented
 * independently in core/src/feature/float_ms_ssim.c (CPU) and
 * core/src/feature/cuda/integer_ms_ssim_cuda.c (CUDA, registered as
 * `float_ms_ssim_cuda`).  Both emit the scalar `float_ms_ssim`
 * feature.  Before this test no cross-backend assertion gated drift;
 * any kernel-grid or SIMD pivot could silently shift the score.
 *
 * The 5-scale MS-SSIM pyramid requires a fixture wide enough that the
 * smallest scale (/16) still has non-trivial dimensions.  256x144
 * gives 16x9 at scale 4 — well above the 11x11 Gaussian window.
 *
 * Asserts agreement to within 1e-4 (places=4, ADR-0214) at frame
 * index 1 across 3 frames, and since ADR-1403 that every output of
 * every frame, the 15 per-scale l / c / s means included, is the
 * CPU's value bit for bit.  Skips cleanly when no CUDA device is
 * visible.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

/* The 5-level 11-tap MS-SSIM pyramid requires min(w,h) >= 11<<4 = 176.
 * 256x192 keeps both axes above the floor with a clean 16-px multiple. */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 192u
#endif
#define FIXTURE_BPC 8u
#define NUM_FRAMES 3u

#define PARITY_TOL 1e-4

static int fill_ref(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + frame_idx * 7u) & 0xFFu);
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

static int fill_dist(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const unsigned base = (row + col + frame_idx * 7u) & 0xFFu;
            const unsigned noise = ((row * 2u + col + frame_idx * 3u) % 9u);
            y[row * pic->stride[0] + col] = (uint8_t)((base + noise) & 0xFFu);
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

static char *run_cpu(bool db, bool identical, double *out_score)
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    VmafFeatureDictionary *opts = NULL;
    if (db) {
        err = ms_ssim_db_opts(&opts);
        mu_assert("CPU: ms_ssim_db_opts failed", !err);
    }
    err = vmaf_use_feature(vmaf, "float_ms_ssim", opts);
    if (err)
        (void)vmaf_feature_dictionary_free(&opts);
    mu_assert("CPU: vmaf_use_feature(float_ms_ssim) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_ref(&ref, i);
        mu_assert("CPU: fill_ref failed", !err);
        err = identical ? fill_ref(&dist, i) : fill_dist(&dist, i);
        mu_assert("CPU: fill_dist failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CPU: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim", out_score, 1u);
    mu_assert("CPU: vmaf_feature_score_at_index(float_ms_ssim, idx=1) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_cuda(bool db, bool identical, double *out_score)
{
    *out_score = NAN;
    int err = 0;

    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    err = vmaf_cuda_state_init(&cu_state, cuda_cfg);
    if (err != 0 || cu_state == NULL) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CUDA: vmaf_init failed", !err);

    err = vmaf_cuda_import_state(vmaf, cu_state);
    mu_assert("CUDA: vmaf_cuda_import_state failed", !err);

    VmafFeatureDictionary *opts = NULL;
    if (db) {
        err = ms_ssim_db_opts(&opts);
        mu_assert("CUDA: ms_ssim_db_opts failed", !err);
    }
    err = vmaf_use_feature(vmaf, "float_ms_ssim_cuda", opts);
    if (err)
        (void)vmaf_feature_dictionary_free(&opts);
    mu_assert("CUDA: vmaf_use_feature(float_ms_ssim_cuda) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_ref(&ref, i);
        mu_assert("CUDA: fill_ref failed", !err);
        err = identical ? fill_ref(&dist, i) : fill_dist(&dist, i);
        mu_assert("CUDA: fill_dist failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CUDA: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CUDA: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim", out_score, 1u);
    mu_assert("CUDA: vmaf_feature_score_at_index(float_ms_ssim, idx=1) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}

static char *test_float_ms_ssim_cpu_cuda_parity(void)
{
    double cpu_score = 0.0;
    double cuda_score = NAN;

    char *msg = run_cpu(false, false, &cpu_score);
    if (msg)
        return msg;
    msg = run_cuda(false, false, &cuda_score);
    if (msg)
        return msg;
    if (isnan(cuda_score))
        return NULL;

    mu_assert("CPU float_ms_ssim score is non-finite", isfinite(cpu_score));
    mu_assert("CUDA float_ms_ssim score is non-finite", isfinite(cuda_score));

    double delta = fabs(cpu_score - cuda_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\nfloat_ms_ssim parity FAIL: cpu=%.8f cuda=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, cuda_score, delta, PARITY_TOL);
    }
    mu_assert("float_ms_ssim CPU vs. CUDA delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* ADR-1221 — clip_db is a CEILING on the dB output, not a clamp on the */
/* linear score.                                                        */
/*                                                                     */
/* float_ms_ssim.c derives `max_db = ceil(10*log10(peak*peak/mse))`     */
/* with `mse = 0.5/(w*h)` and returns                                   */
/* `MIN(-10*log10(1 - score), max_db)`, short-circuiting to `max_db`    */
/* when score >= 1.0. The twin used to clamp the LINEAR score into      */
/* [0, 1] and then convert with no ceiling, which returns +Inf for an   */
/* identical reference/distorted pair.                                  */
/*                                                                     */
/* The fixture feeds the SAME picture as reference and distorted, so    */
/* ms_ssim is 1.0 and the ceiling actually binds. On a merely           */
/* high-similarity pair `-10*log10(1 - score)` stays well below max_db  */
/* and both paths agree, which is why this needs its own fixture rather */
/* than the shared one. Scoring a file against itself is an ordinary    */
/* thing to do, so this is a reachable case, not a synthetic one.       */
/* ------------------------------------------------------------------ */
static char *test_float_ms_ssim_clip_db_ceiling(void)
{
    double cpu_score = 0.0;
    double gpu_score = NAN;

    char *msg = run_cpu(true, true, &cpu_score);
    if (msg)
        return msg;
    msg = run_cuda(true, true, &gpu_score);
    if (msg)
        return msg;
    if (isnan(gpu_score))
        return NULL;

    mu_assert("CPU float_ms_ssim dB score is non-finite", isfinite(cpu_score));
    mu_assert("CUDA float_ms_ssim dB score is non-finite -- clip_db must cap it at max_db",
              isfinite(gpu_score));

    const double delta = fabs(cpu_score - gpu_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\nfloat_ms_ssim enable_db+clip_db parity FAIL: cpu=%.8f cuda=%.8f "
                      "delta=%.2e tol=%.2e\n",
                      cpu_score, gpu_score, delta, PARITY_TOL);
    }
    mu_assert("float_ms_ssim dB score drifts from the CPU reference", delta <= PARITY_TOL);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* ADR-1403 — float_ms_ssim_cuda is the CPU's arithmetic, bit for bit.   */
/*                                                                     */
/* The kernels reproduce ms_ssim_decimate.c (one fused multiply-add per */
/* tap), iqa_convolve() (fp32 products summed in fp64) and              */
/* ssim_accumulate_default_scalar() (fp32 denominators and quotient)    */
/* operation for operation, the fatbin builds without FMA contraction,  */
/* and the host rounds each per-scale mean to fp32 and combines as      */
/* ms_ssim.c does. Before that the twin ran fp32 window sums, fp64      */
/* denominators and unrounded means, and was 2.4e-8 to 3.8e-6 from the  */
/* CPU on the Netflix pair, the checkerboards and BBB 4K; turning       */
/* contraction off alone made it worse at 4K.                           */
/*                                                                     */
/* With enable_lcs the 15 per-scale l / c / s means are compared too:   */
/* every one of the 16 outputs of every frame must equal the CPU's.     */
/* ------------------------------------------------------------------ */
/* NOLINTBEGIN(modernize-use-nullptr) -- ADR-1138: retain NULL for Windows C
 * support and upstream-compatible C test conventions. */
#define MS_EXACT_KEYS 16u

typedef struct MsExactScores {
    double v[NUM_FRAMES][MS_EXACT_KEYS];
} MsExactScores;

static const char *const ms_exact_keys[MS_EXACT_KEYS] = {
    "float_ms_ssim",          "float_ms_ssim_l_scale0", "float_ms_ssim_l_scale1",
    "float_ms_ssim_l_scale2", "float_ms_ssim_l_scale3", "float_ms_ssim_l_scale4",
    "float_ms_ssim_c_scale0", "float_ms_ssim_c_scale1", "float_ms_ssim_c_scale2",
    "float_ms_ssim_c_scale3", "float_ms_ssim_c_scale4", "float_ms_ssim_s_scale0",
    "float_ms_ssim_s_scale1", "float_ms_ssim_s_scale2", "float_ms_ssim_s_scale3",
    "float_ms_ssim_s_scale4",
};

static int ms_exact_feed(VmafContext *vmaf)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_ref(&ref, i);
        if (err)
            return err;
        err = fill_dist(&dist, i);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            return err;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err)
            return err;
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

static int ms_exact_collect(VmafContext *vmaf, MsExactScores *out)
{
    for (unsigned k = 0; k < MS_EXACT_KEYS; k++) {
        for (unsigned i = 0; i < NUM_FRAMES; i++) {
            const int err = vmaf_feature_score_at_index(vmaf, ms_exact_keys[k], &out->v[i][k], i);
            if (err)
                return err;
        }
    }
    return 0;
}

/* Every frame's 16 outputs from one extractor; `cu_state` NULL runs the CPU. */
static int ms_exact_score(VmafCudaState *cu_state, MsExactScores *out)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (err)
        return err;
    if (cu_state)
        err = vmaf_cuda_import_state(vmaf, cu_state);
    VmafFeatureDictionary *opts = NULL;
    if (!err)
        err = vmaf_feature_dictionary_set(&opts, "enable_lcs", "true");
    if (!err) {
        err = vmaf_use_feature(vmaf, cu_state ? "float_ms_ssim_cuda" : "float_ms_ssim", opts);
        opts = err ? opts : NULL; /* taken on success */
    }
    if (opts)
        (void)vmaf_feature_dictionary_free(&opts);
    if (!err)
        err = ms_exact_feed(vmaf);
    if (!err)
        err = ms_exact_collect(vmaf, out);
    const int closed = vmaf_close(vmaf);
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
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        for (unsigned k = 0; k < MS_EXACT_KEYS; k++) {
            if (ms_exact_bits(cpu->v[i][k]) == ms_exact_bits(gpu->v[i][k]))
                continue;
            differing++;
            (void)fprintf(stderr, "\n%s frame %u: cpu=%.17g cuda=%.17g delta=%.3e",
                          ms_exact_keys[k], i, cpu->v[i][k], gpu->v[i][k],
                          fabs(cpu->v[i][k] - gpu->v[i][k]));
        }
    }
    return differing;
}

static char *test_float_ms_ssim_matches_cpu_bit_for_bit(void)
{
    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    if (vmaf_cuda_state_init(&cu_state, cuda_cfg) != 0 || !cu_state) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        return NULL;
    }
    MsExactScores cpu;
    MsExactScores gpu;
    memset(&cpu, 0, sizeof(cpu));
    memset(&gpu, 0, sizeof(gpu));
    const int gpu_err = ms_exact_score(cu_state, &gpu);
    const int freed = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: float_ms_ssim_cuda with enable_lcs failed", gpu_err == 0 && freed == 0);
    mu_assert("CPU: float_ms_ssim with enable_lcs failed", ms_exact_score(NULL, &cpu) == 0);
    mu_assert("CPU float_ms_ssim is not a usable reference",
              isfinite(cpu.v[1][0]) && cpu.v[1][0] > 0.0 && cpu.v[1][0] < 1.0);
    mu_assert("float_ms_ssim_cuda is not the CPU extractor's value bit for bit",
              ms_exact_mismatches(&cpu, &gpu) == 0u);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */

char *run_tests(void)
{
    mu_run_test(test_float_ms_ssim_cpu_cuda_parity);
    mu_run_test(test_float_ms_ssim_clip_db_ceiling);
    mu_run_test(test_float_ms_ssim_matches_cpu_bit_for_bit);
    return NULL;
}
