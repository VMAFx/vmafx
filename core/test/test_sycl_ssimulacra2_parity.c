/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * SYCL kernel coverage round 4 — SSIMULACRA2 CPU vs. SYCL parity test
 * (ADR-0957).
 *
 * The SSIMULACRA2 extractor is implemented by ssimulacra2.c (CPU
 * scalar via vmaf_fex_ssimulacra2) and by
 * ssimulacra2_sycl.cpp::vmaf_fex_ssimulacra2_sycl (ADR-0206; since
 * ADR-1363 the whole frame runs on the device: YUV→linear-RGB,
 * linear-RGB→XYB, the separable Charalampidis 2016 3-pole IIR blur,
 * the per-pixel SSIM/EdgeDiff sums and the 2x2 downsample, with one
 * readback per frame in collect()).
 *
 * Round 3 (ADR-0946) deferred this kernel because the SYCL twin
 * relies on the `yuv_matrix` option-table entry that was not yet
 * templated in the round-2 / round-3 scaffold. Defaults match
 * between CPU and SYCL (`bt709_limited`) so a `NULL` options dict
 * exercises the same numerical path on both sides.
 *
 * Tolerance: 5e-3 — matches the ADR-0214 `FEATURE_TOLERANCE`
 * entry for ssimulacra2 (looser than the places=4 baseline because
 * the multi-stage XYB + IIR + SSIM-combine + log float pipeline
 * accumulates per-stage rounding; ADR-0192 §"Per-feature precision
 * contracts" anticipated places=2 / measure-first).
 *
 * A stride, IIR-coefficient, or XYB-LUT drift in the SYCL kernel
 * would silently shift every SSIMULACRA2 column on Intel-Arc CHUG
 * re-extracts.
 *
 * Headline score asserted: "ssimulacra2" at every one of PARITY_FRAMES
 * frames with distinct content. Several frames matter since ADR-1363:
 * the twin is submit/collect, so frame N is collected after frame N+1
 * is submitted and a mis-keyed readback would score the wrong frame.
 * Measured on Arc B580 / UHD 770 the delta is about 1e-12 (ADR-1363);
 * the assertion stays the ADR-0214 tolerance.
 *
 * Skip behaviour: if vmaf_sycl_state_init() fails (no oneAPI runtime
 * or no device visible) the test emits "[skip: no SYCL device]" and
 * passes, mirroring test_sycl_motion3_parity.c.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* 256x144 matches the round-2 / round-3 fixture footprint and stays
 * comfortably above the 8x8 ssimulacra2 minimum. */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#define FIXTURE_BPC 8u
/* ADR-0214 `FEATURE_TOLERANCE['ssimulacra2'] = 5e-3` — the multi-
 * stage XYB + IIR + SSIM-combine pipeline accumulates per-stage
 * float rounding past the places=4 baseline. */
#define PARITY_TOL 5e-3
#define PARITY_FRAMES 3u

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    /* All three planes carry signal — SSIMULACRA2 consumes YUV →
     * linear-RGB → XYB, so chroma matters for the headline score. */
    for (unsigned p = 0; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                /* XOR + salt + plane-dependent stride keeps every
                 * channel non-trivial. */
                plane[row * pic->stride[p] + col] =
                    (uint8_t)(((row ^ col) + salt * 19u + p * 23u) & 0xFFu);
            }
        }
    }
    return 0;
}

static int feed_frame(VmafContext *vmaf, unsigned index)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_pic(&ref, 2u * index);
    if (err)
        return err;
    err = fill_pic(&dist, 2u * index + 1u + index * 5u);
    if (err) {
        vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, index);
}

static int feed_frames(VmafContext *vmaf)
{
    for (unsigned i = 0; i < PARITY_FRAMES; i++) {
        const int err = feed_frame(vmaf, i);
        if (err)
            return err;
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

static int read_scores(VmafContext *vmaf, double scores[PARITY_FRAMES])
{
    for (unsigned i = 0; i < PARITY_FRAMES; i++) {
        const int err = vmaf_feature_score_at_index(vmaf, "ssimulacra2", &scores[i], i);
        if (err)
            return err;
    }
    return 0;
}

static char *run_cpu(double score[PARITY_FRAMES])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "ssimulacra2", NULL);
    mu_assert("CPU: vmaf_use_feature(ssimulacra2) failed", !err);
    err = feed_frames(vmaf);
    mu_assert("CPU: feeding frames failed", !err);
    err = read_scores(vmaf, score);
    mu_assert("CPU: ssimulacra2 score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_sycl(double score[PARITY_FRAMES], int *device_present)
{
    for (unsigned i = 0; i < PARITY_FRAMES; i++)
        score[i] = NAN;
    *device_present = 0;
    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    int err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err != 0 || sycl_state == NULL) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return NULL;
    }
    *device_present = 1;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("SYCL: vmaf_init failed", !err);
    err = vmaf_sycl_import_state(vmaf, sycl_state);
    mu_assert("SYCL: vmaf_sycl_import_state failed", !err);
    err = vmaf_use_feature(vmaf, "ssimulacra2_sycl", NULL);
    mu_assert("SYCL: vmaf_use_feature(ssimulacra2_sycl) failed", !err);
    err = feed_frames(vmaf);
    mu_assert("SYCL: feeding frames failed", !err);
    err = read_scores(vmaf, score);
    mu_assert("SYCL: ssimulacra2 score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);
    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

static char *test_ssimulacra2_sycl_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ssimulacra2_sycl");
    mu_assert("ssimulacra2_sycl extractor must be registered", fex != NULL);
    mu_assert("ssimulacra2_sycl name matches", !strcmp(fex->name, "ssimulacra2_sycl"));
    return NULL;
}

static char *test_ssimulacra2_cpu_sycl_parity(void)
{
    double cpu_score[PARITY_FRAMES] = {0.0};
    double sycl_score[PARITY_FRAMES] = {0.0};
    int device_present = 0;
    char *msg = run_cpu(cpu_score);
    if (msg)
        return msg;
    msg = run_sycl(sycl_score, &device_present);
    if (msg)
        return msg;
    if (!device_present)
        return NULL;
    for (unsigned i = 0; i < PARITY_FRAMES; i++) {
        const double delta = fabs(cpu_score[i] - sycl_score[i]);
        if (!(delta <= PARITY_TOL)) {
            (void)fprintf(stderr,
                          "\nssimulacra2 parity FAIL: frame=%u cpu=%.12f sycl=%.12f delta=%.2e "
                          "tol=%.2e\n",
                          i, cpu_score[i], sycl_score[i], delta, PARITY_TOL);
        }
        mu_assert("ssimulacra2 CPU vs. SYCL delta exceeds ADR-0214 ssimulacra2 tolerance (5e-3)",
                  delta <= PARITY_TOL);
    }
    /* Distinct content per frame: equal neighbours would hide a readback
     * keyed to the wrong frame index. */
    mu_assert("ssimulacra2 fixture frames must score differently",
              cpu_score[0] != cpu_score[1] && cpu_score[1] != cpu_score[2]);
    return NULL;
}

/* ADR-1324 / ADR-1359: inputs the twin's init rejects go to the CPU extractor
 * instead. Boundaries: 8x8 is the smallest accepted frame; 4:0:0 has no
 * chroma to convert. Needs no device. */
static char *test_ssimulacra2_sycl_context_check(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ssimulacra2_sycl");
    mu_assert("ssimulacra2_sycl extractor must be registered", fex != NULL);
    mu_assert("ssimulacra2_sycl declares a context check", fex->context_check != NULL);
    mu_assert("ssimulacra2_sycl falls back to the CPU extractor",
              fex->context_fallback_name && !strcmp(fex->context_fallback_name, "ssimulacra2"));
    return NULL;
}

static char *test_ssimulacra2_sycl_context_bounds(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ssimulacra2_sycl");
    mu_assert("ssimulacra2_sycl declares a context check", fex && fex->context_check);
    mu_assert("8x8 4:2:0 is accepted",
              fex->context_check(fex, VMAF_PIX_FMT_YUV420P, 8u, 8u, 8u) == 0);
    mu_assert("4K 4:4:4 10-bit is accepted",
              fex->context_check(fex, VMAF_PIX_FMT_YUV444P, 10u, 3840u, 2160u) == 0);
    mu_assert("7x8 is rejected",
              fex->context_check(fex, VMAF_PIX_FMT_YUV420P, 8u, 7u, 8u) == -ENOTSUP);
    mu_assert("8x7 is rejected",
              fex->context_check(fex, VMAF_PIX_FMT_YUV422P, 8u, 8u, 7u) == -ENOTSUP);
    mu_assert("4:0:0 is rejected",
              fex->context_check(fex, VMAF_PIX_FMT_YUV400P, 8u, 576u, 324u) == -ENOTSUP);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_ssimulacra2_sycl_registered);
    mu_run_test(test_ssimulacra2_sycl_context_check);
    mu_run_test(test_ssimulacra2_sycl_context_bounds);
    mu_run_test(test_ssimulacra2_cpu_sycl_parity);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
