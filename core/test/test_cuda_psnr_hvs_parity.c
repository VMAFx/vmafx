/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Round-2 GPU-kernel coverage gap-fill — psnr_hvs CPU vs. CUDA parity.
 *
 * PSNR-HVS is a perceptual peak-signal-to-noise variant that applies the
 * HVS contrast-sensitivity weighting before the MSE reduction. CPU is in
 * third_party/xiph/psnr_hvs.c (Xiph reference port); CUDA path is in
 * integer_psnr_hvs_cuda.c + integer_psnr_hvs/psnr_hvs_score.cu.
 *
 * Both backends emit psnr_hvs_y / psnr_hvs_cb / psnr_hvs_cr per-plane plus
 * a combined psnr_hvs. Per the kernel header the CUDA path runs all three
 * planes; cross-checking the combined score is the highest-coverage
 * single assertion.
 *
 * Round-1 (PR #351) covered psnr_cuda + ciede_cuda — neither exercises the
 * HVS-weighted DCT-coefficient path.
 *
 * Skip behaviour: skips with "[skip: no CUDA device]" when no CUDA driver.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#define FIXTURE_BPC 8u

#define PARITY_TOL 1e-4

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row * 5u + col + salt * 11u) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                plane[row * pic->stride[p] + col] =
                    (uint8_t)((row + col * (1u + p) + salt * 7u) & 0xFFu);
            }
        }
    }
    return 0;
}

static char *feed_one_frame(VmafContext *vmaf)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_pic(&ref, 0u);
    mu_assert("fill_pic(ref) failed", !err);
    err = fill_pic(&dist, 1u);
    mu_assert("fill_pic(dist) failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    mu_assert("vmaf_read_pictures failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("vmaf_read_pictures(EOS) failed", !err);
    return NULL;
}

static char *run_cpu(double *score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "psnr_hvs", NULL);
    mu_assert("CPU: vmaf_use_feature(psnr_hvs) failed", !err);

    char *msg = feed_one_frame(vmaf);
    if (msg)
        return msg;

    err = vmaf_feature_score_at_index(vmaf, "psnr_hvs", score, 0u);
    mu_assert("CPU: vmaf_feature_score_at_index(psnr_hvs) failed", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_cuda(double *score)
{
    *score = NAN;
    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    int err = vmaf_cuda_state_init(&cu_state, cuda_cfg);
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

    err = vmaf_use_feature(vmaf, "psnr_hvs_cuda", NULL);
    mu_assert("CUDA: vmaf_use_feature(psnr_hvs_cuda) failed", !err);

    char *msg = feed_one_frame(vmaf);
    if (msg)
        return msg;

    err = vmaf_feature_score_at_index(vmaf, "psnr_hvs", score, 0u);
    mu_assert("CUDA: vmaf_feature_score_at_index(psnr_hvs) failed", !err);
    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}

static char *test_psnr_hvs_cuda_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr_hvs_cuda");
    mu_assert("psnr_hvs_cuda extractor must be registered", fex != NULL);
    mu_assert("psnr_hvs_cuda name matches", !strcmp(fex->name, "psnr_hvs_cuda"));
    return NULL;
}

static char *test_psnr_hvs_cpu_cuda_parity(void)
{
    double cpu = 0.0;
    double gpu = NAN;

    char *msg = run_cpu(&cpu);
    if (msg)
        return msg;
    msg = run_cuda(&gpu);
    if (msg)
        return msg;
    if (isnan(gpu))
        return NULL;

    const double delta = fabs(cpu - gpu);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\npsnr_hvs parity FAIL: cpu=%.8f cuda=%.8f delta=%.2e tol=%.2e\n",
                      cpu, gpu, delta, PARITY_TOL);
    }
    mu_assert("psnr_hvs CPU vs. CUDA delta exceeds places=4 tolerance (1e-4)", delta <= PARITY_TOL);
    return NULL;
}

/* 9- and 11-bit input: calc_psnrhvs() uses the raw sample at every depth. The
 * twin used to convert only 10- and 12-bit samples back exactly and scored
 * 9 / 11 bits on 16 times the sample (-1.57 against 22.47 dB at 9 bits).
 * 4:0:0 input: the CPU extractor scores luma only; the twin must do the same
 * instead of refusing the format. */
static void fill_deep_row(uint8_t *line, unsigned width, unsigned row, unsigned plane, unsigned bpc,
                          unsigned salt)
{
    const unsigned range = 1u << bpc;
    for (unsigned col = 0; col < width; col++) {
        const unsigned v = ((col * 37u + row * 11u + plane * 5u) ^ (salt * 91u)) % range;
        if (bpc > 8u) {
            ((uint16_t *)line)[col] = (uint16_t)v;
        } else {
            line[col] = (uint8_t)v;
        }
    }
}

static int fill_deep_pic(VmafPicture *pic, enum VmafPixelFormat fmt, unsigned bpc, unsigned salt)
{
    const int err = vmaf_picture_alloc(pic, fmt, bpc, 64u, 48u);
    if (err)
        return err;
    const unsigned planes = (fmt == VMAF_PIX_FMT_YUV400P) ? 1u : 3u;
    for (unsigned p = 0; p < planes; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            uint8_t *line = (uint8_t *)pic->data[p] + (size_t)row * pic->stride[p];
            fill_deep_row(line, pic->w[p], row, p, bpc, salt);
        }
    }
    return 0;
}

static char *open_deep(VmafCudaState *cuda_state, VmafContext **vmaf)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    mu_assert("vmaf_init failed", !vmaf_init(vmaf, cfg));
    if (cuda_state)
        mu_assert("vmaf_cuda_import_state failed", !vmaf_cuda_import_state(*vmaf, cuda_state));
    mu_assert("vmaf_use_feature failed",
              !vmaf_use_feature(*vmaf, cuda_state ? "psnr_hvs_cuda" : "psnr_hvs", NULL));
    return NULL;
}

static char *score_deep(VmafCudaState *cuda_state, enum VmafPixelFormat fmt, unsigned bpc,
                        double *score)
{
    VmafContext *vmaf = NULL;
    mu_assert_msg(open_deep(cuda_state, &vmaf));
    VmafPicture ref;
    VmafPicture dist;
    mu_assert("ref alloc", !fill_deep_pic(&ref, fmt, bpc, 0u));
    mu_assert("dist alloc", !fill_deep_pic(&dist, fmt, bpc, 1u));
    mu_assert("read", !vmaf_read_pictures(vmaf, &ref, &dist, 0u));
    mu_assert("flush", !vmaf_read_pictures(vmaf, NULL, NULL, 0));
    mu_assert("score", !vmaf_feature_score_at_index(vmaf, "psnr_hvs", score, 0u));
    mu_assert("vmaf_close failed", !vmaf_close(vmaf));
    return NULL;
}

/* One CPU-vs-CUDA comparison of the combined psnr_hvs on a 64x48 picture. */
static char *compare_deep(enum VmafPixelFormat fmt, unsigned bpc)
{
    VmafCudaState *cuda_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    if (vmaf_cuda_state_init(&cuda_state, cuda_cfg) != 0 || cuda_state == NULL) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        return NULL;
    }
    double cpu = 0.0;
    double gpu = NAN;
    char *msg = score_deep(NULL, fmt, bpc, &cpu);
    if (!msg)
        msg = score_deep(cuda_state, fmt, bpc, &gpu);
    if (!msg && !(fabs(cpu - gpu) <= PARITY_TOL)) {
        (void)fprintf(stderr, "\nformat %d, %u-bit psnr_hvs: cpu=%.8f cuda=%.8f\n", (int)fmt, bpc,
                      cpu, gpu);
        msg = "psnr_hvs_cuda must score the raw samples of every plane the CPU scores";
    }
    const int free_err = vmaf_cuda_state_free(cuda_state);
    if (!msg && free_err)
        msg = "vmaf_cuda_state_free failed";
    return msg;
}

static char *test_psnr_hvs_odd_depth_parity(void)
{
    char *msg = compare_deep(VMAF_PIX_FMT_YUV420P, 9u);
    return msg ? msg : compare_deep(VMAF_PIX_FMT_YUV420P, 11u);
}

static char *test_psnr_hvs_yuv400_parity(void)
{
    return compare_deep(VMAF_PIX_FMT_YUV400P, 8u);
}

char *run_tests(void)
{
    mu_run_test(test_psnr_hvs_cuda_registered);
    mu_run_test(test_psnr_hvs_cpu_cuda_parity);
    mu_run_test(test_psnr_hvs_odd_depth_parity);
    mu_run_test(test_psnr_hvs_yuv400_parity);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
