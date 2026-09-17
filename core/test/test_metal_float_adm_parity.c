/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Metal kernel parity — float_adm CPU vs. Metal (ADR-0214 cross-backend
 * gate; ADR-0421 Metal kernel scaffolding).
 *
 * The float-path ADM extractor is implemented independently in
 * core/src/feature/float_adm.c (CPU) and
 * core/src/feature/metal/float_adm_metal.mm (Metal). Both emit the
 * `VMAF_feature_adm2_score` aggregate plus the four `adm_scale[0..3]`
 * sub-scores. The Metal twin ports the CUDA twin's six-stage
 * DWT → CSF → decouple → CM pipeline (float_adm_cuda.c +
 * float_adm/float_adm_score.cu) to MSL: per-scale DWT pyramid band
 * buffers, fused CSF-denominator + cross-band CM threshold, host-side
 * double-precision reduction with cube-root pooling.
 *
 * Tolerance rationale: places=4 (1e-4), the same ADR-0214 cross-backend
 * bound the CUDA twin holds (see core/test/test_cuda_float_adm_parity.c).
 * The per-pixel arithmetic is float32 on both GPU and the CPU's float
 * path; the per-band reductions promote to double on both sides, so the
 * residual divergence is float-vs-float rounding-order noise bounded by
 * 1e-4. Only csf_mode 0 (Watson-97, the CPU default) is exercised —
 * float_adm_metal returns -EINVAL for other modes.
 *
 * Fixture: 256x144 YUV420P 8-bpc, 3 frames, deterministic ref/dist
 * ramps that differ so the ADM score is non-trivial and finite. The
 * 4-scale DWT pyramid bottoms out at 32x18 (256/8 x 144/8), comfortably
 * above the 1-px floor. We read the score at frame index 1 (mirrors
 * test_cuda_float_adm_parity.c).
 *
 * Skip behaviour: -ENODEV from `vmaf_metal_state_init` -> clean skip on
 * Linux / Windows / Intel Mac.
 *
 * Cross-references:
 *   - core/src/feature/metal/float_adm_metal.mm
 *   - core/src/feature/float_adm.c
 *   - core/test/test_cuda_float_adm_parity.c (CUDA twin parity)
 *   - core/test/test_metal_float_ms_ssim_parity.c (sibling multi-scale)
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_metal.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u
#define NUM_FRAMES 3u

/* ADR-0214 cross-backend tolerance (places=4 -> 1e-4). */
#define PARITY_TOL 1e-4

#define NUM_ADM_FEATURES 5u
static const char *const ADM_FEATURES[NUM_ADM_FEATURES] = {
    "VMAF_feature_adm2_score",       "VMAF_feature_adm_scale0_score",
    "VMAF_feature_adm_scale1_score", "VMAF_feature_adm_scale2_score",
    "VMAF_feature_adm_scale3_score",
};

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
            const unsigned noise = ((row * 3u + col * 2u + frame_idx) % 13u);
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

/* Both sides feed the same fixture pair, so the sequence lives here once.
 * Extracting it also keeps each run_* function inside the branch budget the
 * lint profile sets, which is the refactor ADR-0141 asks for rather than a
 * suppression. */
/* Reading every feature is the same loop on both sides; extracting it keeps
 * each run_* function inside the lint profile's branch budget (ADR-0141 asks
 * for the refactor rather than a suppression). */
static char *read_feature_scores(VmafContext *vmaf, const char *const *names, unsigned count,
                                 double *out_scores, unsigned index)
{
    for (unsigned i = 0; i < count; i++) {
        const int err = vmaf_feature_score_at_index(vmaf, names[i], &out_scores[i], index);
        if (err)
            return "vmaf_feature_score_at_index failed";
    }
    return NULL;
}

static char *feed_fixture_pair(VmafContext *vmaf, unsigned frame)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_ref(&ref, frame);
    if (err)
        return "fill_ref failed";
    err = fill_dist(&dist, frame);
    if (err)
        return "fill_dist failed";
    err = vmaf_read_pictures(vmaf, &ref, &dist, frame);
    if (err)
        return "vmaf_read_pictures failed";
    return NULL;
}

static char *run_cpu(double *out_scores)
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "float_adm", NULL);
    mu_assert("CPU: vmaf_use_feature(float_adm) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        char *feed_err = feed_fixture_pair(vmaf, i);
        if (feed_err)
            return feed_err;
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    char *score_err = read_feature_scores(vmaf, ADM_FEATURES, NUM_ADM_FEATURES,
                                          out_scores, 1u);
    if (score_err)
        return score_err;

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_metal(double *out_scores, int *skipped)
{
    *skipped = 0;
    for (unsigned m = 0; m < NUM_ADM_FEATURES; m++)
        out_scores[m] = NAN;

    int err = 0;
    VmafMetalConfiguration mcfg = {.device_index = -1, .flags = 0};
    VmafMetalState *mstate = NULL;
    err = vmaf_metal_state_init(&mstate, mcfg);
    if (err != 0 || mstate == NULL) {
        (void)fprintf(stderr, "[skip: no Metal device] ");
        *skipped = 1;
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("Metal: vmaf_init failed", !err);

    err = vmaf_metal_import_state(vmaf, mstate);
    mu_assert("Metal: vmaf_metal_import_state failed", !err);

    err = vmaf_use_feature(vmaf, "float_adm_metal", NULL);
    mu_assert("Metal: vmaf_use_feature(float_adm_metal) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        char *feed_err = feed_fixture_pair(vmaf, i);
        if (feed_err)
            return feed_err;
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("Metal: vmaf_read_pictures(EOS) failed", !err);

    char *score_err = read_feature_scores(vmaf, ADM_FEATURES, NUM_ADM_FEATURES,
                                          out_scores, 1u);
    if (score_err)
        return score_err;

    err = vmaf_close(vmaf);
    mu_assert("Metal: vmaf_close failed", !err);
    vmaf_metal_state_free(&mstate);
    return NULL;
}

static char *test_float_adm_cpu_metal_parity(void)
{
    double cpu_scores[NUM_ADM_FEATURES] = {0};
    double metal_scores[NUM_ADM_FEATURES] = {0};
    int skipped = 0;

    char *msg = run_cpu(cpu_scores);
    if (msg)
        return msg;
    msg = run_metal(metal_scores, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;

    for (unsigned m = 0; m < NUM_ADM_FEATURES; m++) {
        mu_assert("CPU float_adm score is non-finite", isfinite(cpu_scores[m]));
        mu_assert("Metal float_adm score is non-finite", isfinite(metal_scores[m]));

        const double delta = fabs(cpu_scores[m] - metal_scores[m]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nfloat_adm parity FAIL %s: cpu=%.8f metal=%.8f delta=%.2e tol=%.2e\n",
                          ADM_FEATURES[m], cpu_scores[m], metal_scores[m], delta, PARITY_TOL);
        }
        mu_assert("float_adm CPU vs. Metal delta exceeds places=4 tolerance (1e-4)",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_adm_cpu_metal_parity);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
