/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Small-frame ADM parity test — exercises the `i == 0 && top <= 0` border branch.
 *
 * Scale 3 height must be <= 14 px to trigger `top <= 0` (with ADM_BORDER_FACTOR=0.07:
 * 14 * 0.07 - 0.5 = 0.48 -> 0). With FIXTURE_H = 96, scale 3 has height = 12 <= 14,
 * which sets top = 0, start_row = 0, and enters the `i == 0 && top <= 0` branch.
 *
 * The pre-fix CUDA/HIP kernels walked running pointers reading rows {1, 2, 3} and
 * sampled csf_a at row 2 instead of row 0. The absolute-indexing fix restores
 * parity with CPU reference rows {1, 0, 1} and row 0 center csf_a.
 *
 * Fixture: the lowbias32 texture in luma_sample(). Measured on a gfx1036 with
 * that border defect planted back into adm_cm.hip: the earlier smooth ramp
 * moved adm2 by 7.5e-6 (under the 1e-4 gate), the texture moves it by 4.0e-4.
 */

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

#if defined(HAVE_CUDA)
#include "libvmaf/libvmaf_cuda.h"
#define GPU_BACKEND_NAME "adm_cuda"
#elif defined(HAVE_HIP)
#include "libvmaf/libvmaf_hip.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */
#define GPU_BACKEND_NAME "adm_hip"
#endif

/* Small fixture: H = 96 px -> scale 3 has H = 12 px (<= 14 px). */
#define FIXTURE_W 160u
#define FIXTURE_H 96u
#define FIXTURE_BPC 8u

#define PARITY_TOL 1e-4

static const char *const ADM_FEATURES[] = {
    "VMAF_integer_feature_adm2_score",
#if !defined(HAVE_HIP)
    /* The HIP twin does not emit adm3_score (docs/metrics/features.md). */
    "VMAF_integer_feature_adm3_score",
#endif
    "integer_adm_scale3",
};
#define NUM_ADM_FEATURES (sizeof(ADM_FEATURES) / sizeof(ADM_FEATURES[0]))

/* lowbias32 of the position, seeded per picture. Stateless, so every backend
 * scores the same frames. */
static uint32_t position_hash(unsigned row, unsigned col, uint32_t seed)
{
    uint32_t x = ((uint32_t)row << 16) ^ (uint32_t)col ^ seed;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* Full-range 8-bit noise for the reference; the distorted picture adds an
 * independent perturbation in [-16, 15]. Every DWT band then carries energy
 * at every scale and the pair stays correlated (adm2 in (0.5, 1)). A smooth
 * ramp gives the contrast-masking kernel almost nothing to accumulate: with
 * the earlier `(row * 7 + col * 5) & 0xFF` ramp the pre-ADR-1167 border and
 * rounding defects moved the scores by at most 7.5e-6, under the 1e-4 gate. */
static uint8_t luma_sample(unsigned row, unsigned col, int distorted)
{
    const int ref = (int)(position_hash(row, col, 0x85EBCA6Bu) >> 24);
    if (!distorted)
        return (uint8_t)ref;
    const int delta = (int)(position_hash(row, col, 0x9E3779B9u) >> 27) - 16;
    const int v = ref + delta;
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static int fill_picture(VmafPicture *pic, int distorted)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    assert(pic->data[0] != NULL);
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = luma_sample(row, col, distorted);
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

static char *feed_one_frame(VmafContext *vmaf)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_picture(&ref, 0);
    mu_assert("fill_picture(ref) failed", !err);
    err = fill_picture(&dist, 1);
    mu_assert("fill_picture(dist) failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    mu_assert("vmaf_read_pictures failed", !err);
    return NULL;
}

/* Reading every ADM feature is the same loop in each backend arm; folding it
 * into a helper keeps run_gpu_adm inside the lint profile's branch budget
 * (ADR-0141 asks for the refactor rather than a suppression). */
static char *read_adm_scores(VmafContext *vmaf, double scores_out[NUM_ADM_FEATURES], unsigned index)
{
    for (unsigned k = 0; k < NUM_ADM_FEATURES; k++) {
        const int err = vmaf_feature_score_at_index(vmaf, ADM_FEATURES[k], &scores_out[k], index);
        if (err) {
            (void)fprintf(stderr, "\nvmaf_feature_score_at_index(\"%s\", %u) failed: %d\n",
                          ADM_FEATURES[k], index, err);
            return "vmaf_feature_score_at_index failed";
        }
    }
    return NULL;
}

static char *run_cpu_adm(double scores_out[NUM_ADM_FEATURES])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "adm", NULL);
    mu_assert("CPU: vmaf_use_feature(adm) failed", !err);

    char *msg = feed_one_frame(vmaf);
    if (msg)
        return msg;
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    char *score_err = read_adm_scores(vmaf, scores_out, 0u);
    if (score_err)
        return score_err;

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

#if defined(HAVE_CUDA)
/* One function per backend arm: each is a linear setup sequence with an
 * assertion per call, and keeping them separate is what holds each inside the
 * lint profile's branch budget (ADR-0141 asks for the refactor). */
static char *run_cuda_adm(double scores_out[NUM_ADM_FEATURES])
{
    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    int err = vmaf_cuda_state_init(&cu_state, cuda_cfg);
    if (err != 0 || cu_state == NULL) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        mu_skipped = 1;
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CUDA: vmaf_init failed", !err);

    err = vmaf_cuda_import_state(vmaf, cu_state);
    mu_assert("CUDA: vmaf_cuda_import_state failed", !err);

    err = vmaf_use_feature(vmaf, GPU_BACKEND_NAME, NULL);
    mu_assert("CUDA: vmaf_use_feature failed", !err);

    char *msg = feed_one_frame(vmaf);
    if (msg)
        return msg;
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CUDA: vmaf_read_pictures(EOS) failed", !err);

    char *score_err = read_adm_scores(vmaf, scores_out, 0u);
    if (score_err)
        return score_err;

    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}
#elif defined(HAVE_HIP)
/* feed_one_frame for the HIP arm, which skips rather than fails when the HIP
 * kernels were not built: vmaf_read_pictures then reports -ENOSYS and
 * *skipped tells the caller to tear down and skip. */
static char *hip_feed_one_frame(VmafContext *vmaf, int *skipped)
{
    VmafPicture ref;
    VmafPicture dist;
    *skipped = 0;
    int err = fill_picture(&ref, 0);
    mu_assert("fill_picture(ref) failed", !err);
    err = fill_picture(&dist, 1);
    mu_assert("fill_picture(dist) failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    if (err == -ENOSYS) {
        *skipped = 1;
        return NULL;
    }
    mu_assert("HIP: vmaf_read_pictures failed", !err);
    return NULL;
}

static char *run_hip_adm(double scores_out[NUM_ADM_FEATURES])
{
    VmafHipState *hip_state = NULL;
    VmafHipConfiguration hip_cfg = {0};
    int err = vmaf_hip_state_init(&hip_state, hip_cfg);
    if (err != 0 || hip_state == NULL) {
        (void)fprintf(stderr, "[skip: no HIP device] ");
        mu_skipped = 1;
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("HIP: vmaf_init failed", !err);

    err = vmaf_hip_import_state(vmaf, hip_state);
    mu_assert("HIP: vmaf_hip_import_state failed", !err);

    err = vmaf_use_feature(vmaf, GPU_BACKEND_NAME, NULL);
    mu_assert("HIP: vmaf_use_feature failed", !err);

    int skipped = 0;
    char *msg = hip_feed_one_frame(vmaf, &skipped);
    if (msg)
        return msg;
    if (skipped) {
        (void)fprintf(stderr, "[skip: HIP kernels not built] ");
        mu_skipped = 1;
        (void)vmaf_close(vmaf);
        vmaf_hip_state_free(&hip_state);
        return NULL;
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("HIP: vmaf_read_pictures(EOS) failed", !err);

    char *score_err = read_adm_scores(vmaf, scores_out, 0u);
    if (score_err)
        return score_err;

    err = vmaf_close(vmaf);
    mu_assert("HIP: vmaf_close failed", !err);
    vmaf_hip_state_free(&hip_state);
    return NULL;
}
#endif

static char *run_gpu_adm(double scores_out[NUM_ADM_FEATURES])
{
    for (unsigned k = 0; k < NUM_ADM_FEATURES; k++)
        scores_out[k] = NAN;

#if defined(HAVE_CUDA)
    return run_cuda_adm(scores_out);
#elif defined(HAVE_HIP)
    return run_hip_adm(scores_out);
#else
    return NULL;
#endif
}

static char *test_small_border_parity(void)
{
    double cpu[NUM_ADM_FEATURES] = {0.0};
    double gpu[NUM_ADM_FEATURES] = {NAN};

    char *msg = run_cpu_adm(cpu);
    if (msg)
        return msg;
    msg = run_gpu_adm(gpu);
    if (msg)
        return msg;
    if (isnan(gpu[0]))
        return NULL; /* skipped */

    for (unsigned k = 0; k < NUM_ADM_FEATURES; k++) {
        const double delta = fabs(cpu[k] - gpu[k]);
        (void)fprintf(stderr, "\n%s: cpu=%.8f gpu=%.8f delta=%.2e", ADM_FEATURES[k], cpu[k], gpu[k],
                      delta);
        if (delta > PARITY_TOL) {
            (void)fprintf(
                stderr,
                "\nsmall border ADM parity FAIL (%s): cpu=%.8f gpu=%.8f delta=%.2e tol=%.2e\n",
                ADM_FEATURES[k], cpu[k], gpu[k], delta, PARITY_TOL);
        }
        mu_assert("small border ADM CPU vs. GPU delta exceeds places=4 tolerance (1e-4)",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_small_border_parity);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
