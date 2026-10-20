/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ciede2000 CPU vs. CUDA parity (GPU-kernel coverage gap-fill; the CPU's
 * arithmetic since ADR-1426).
 *
 * The CIEDE2000 colour difference is ciede.c on the CPU and
 * integer_ciede_cuda.c plus integer_ciede/ciede_score.cu on CUDA. Since
 * ADR-1426 the kernel evaluates the reference's expressions in the
 * reference's types (integer_ciede/ciede_device.h) and the host adds the
 * per-pixel values in the reference's raster order. What still differs is
 * the math library: the CPU calls glibc, the device CUDA's functions, and a
 * few pixels in a million round to the neighbouring float (glibc's powf is
 * not correctly rounded; the fp64 functions differ in their last place).
 *
 * So this test asserts a tolerance, and a tight one. A pixel whose value is
 * one float step off moves `45 - 20 * log10(mean)` by at most
 * 8.7 * 1.2e-7 * (value / mean) / pixels: 2.8e-11 times the pixel's weight
 * at 256x144. 1e-8 leaves room for many such pixels; before ADR-1426 the
 * twin computed in fp32 with another form of the formula and was 7e-8 to
 * 3e-7 away on these fixtures (1e-5 on video), so every case below fails on
 * it.
 *
 * Skip behaviour: if vmaf_cuda_state_init() fails (no CUDA driver or
 * no device visible) the test emits "[skip: no CUDA device]" and passes.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif

#define PARITY_TOL 1e-8

typedef struct Case {
    const char *what;
    unsigned w;
    unsigned h;
    unsigned bpc;
    enum VmafPixelFormat pix_fmt;
} Case;

/* Deterministic position hash. */
static unsigned position_hash(unsigned plane, unsigned row, unsigned col, unsigned salt)
{
    unsigned x = row * 73856093u ^ col * 19349663u ^ (salt * 4u + plane) * 83492791u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return x;
}

/* Sample in 8-bit levels: every plane varies, and the distorted picture is
 * the reference plus an error of a few levels, so the pixel differences span
 * near-neutral and saturated colours and small and large hue differences. */
static unsigned fixture_sample(unsigned plane, unsigned row, unsigned col, bool distorted)
{
    const unsigned lo = (plane == 0u) ? 16u : 40u;
    const unsigned span = (plane == 0u) ? 220u : 176u;
    unsigned v = lo + position_hash(plane, row >> 1, col >> 1, 1u) % span;
    if (distorted)
        v += position_hash(plane, row, col, 2u) % 9u;
    return v;
}

static void put_sample(VmafPicture *pic, unsigned plane, unsigned row, unsigned col, unsigned v)
{
    uint8_t *line = (uint8_t *)pic->data[plane] + (size_t)row * (size_t)pic->stride[plane];
    if (pic->bpc <= 8u) {
        line[col] = (uint8_t)v;
    } else {
        ((uint16_t *)line)[col] = (uint16_t)v;
    }
}

static int fill_picture(VmafPicture *pic, const Case *c, bool distorted)
{
    int err = vmaf_picture_alloc(pic, c->pix_fmt, c->bpc, c->w, c->h);
    if (err)
        return err;
    const unsigned gain = 1u << (c->bpc - 8u);
    for (unsigned p = 0; p < 3; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++)
                put_sample(pic, p, row, col, fixture_sample(p, row, col, distorted) * gain);
        }
    }
    return 0;
}

static char *feed_one_frame(VmafContext *vmaf, const Case *c)
{
    VmafPicture ref;
    VmafPicture dist;
    mu_assert("fill reference failed", !fill_picture(&ref, c, false));
    mu_assert("fill distorted failed", !fill_picture(&dist, c, true));
    mu_assert("vmaf_read_pictures failed", !vmaf_read_pictures(vmaf, &ref, &dist, 0u));
    mu_assert("vmaf_read_pictures(EOS) failed", !vmaf_read_pictures(vmaf, NULL, NULL, 0));
    return NULL;
}

/* Score the case with one extractor. `cu_state` is NULL for the CPU. */
static char *score_case(const Case *c, VmafCudaState *cu_state, double *score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init failed", !vmaf_init(&vmaf, cfg));
    if (cu_state)
        mu_assert("vmaf_cuda_import_state failed", !vmaf_cuda_import_state(vmaf, cu_state));
    mu_assert("vmaf_use_feature failed",
              !vmaf_use_feature(vmaf, cu_state ? "ciede_cuda" : "ciede", NULL));
    char *msg = feed_one_frame(vmaf, c);
    if (msg)
        return msg;
    mu_assert("vmaf_feature_score_at_index(ciede2000) failed",
              !vmaf_feature_score_at_index(vmaf, "ciede2000", score, 0u));
    mu_assert("vmaf_close failed", !vmaf_close(vmaf));
    return NULL;
}

/* The CUDA twin's score is within PARITY_TOL of the CPU's. A missing device
 * skips the case. */
static char *check_case(const Case *c)
{
    double cpu = 0.0;
    double gpu = NAN;
    char *msg = score_case(c, NULL, &cpu);
    if (msg)
        return msg;

    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    if (vmaf_cuda_state_init(&cu_state, cuda_cfg) != 0 || cu_state == NULL) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        mu_skipped = 1;
        return NULL;
    }
    msg = score_case(c, cu_state, &gpu);
    mu_assert("vmaf_cuda_state_free failed", !vmaf_cuda_state_free(cu_state));
    if (msg)
        return msg;

    mu_assert("CPU ciede2000 is not finite", isfinite(cpu));
    const double delta = fabs(cpu - gpu);
    if (!(delta <= PARITY_TOL)) {
        (void)fprintf(stderr, "\n%s %ux%u %u-bit: cpu=%.17g cuda=%.17g delta=%.3e tol=%.1e\n",
                      c->what, c->w, c->h, c->bpc, cpu, gpu, delta, PARITY_TOL);
    }
    mu_assert("ciede_cuda is further from the CPU extractor than its math library explains",
              delta <= PARITY_TOL);
    return NULL;
}

static char *test_ciede_cuda_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ciede_cuda");
    mu_assert("ciede_cuda extractor must be registered", fex != NULL);
    mu_assert("ciede_cuda name matches", !strcmp(fex->name, "ciede_cuda"));
    return NULL;
}

static char *test_ciede_8bit(void)
{
    const Case c = {"ciede 4:2:0", FIXTURE_W, FIXTURE_H, 8u, VMAF_PIX_FMT_YUV420P};
    return check_case(&c);
}

static char *test_ciede_10bit(void)
{
    const Case c = {"ciede 4:2:0", FIXTURE_W, FIXTURE_H, 10u, VMAF_PIX_FMT_YUV420P};
    return check_case(&c);
}

static char *test_ciede_12bit(void)
{
    const Case c = {"ciede 4:2:0", FIXTURE_W, FIXTURE_H, 12u, VMAF_PIX_FMT_YUV420P};
    return check_case(&c);
}

static char *test_ciede_16bit(void)
{
    const Case c = {"ciede 4:2:0", FIXTURE_W, FIXTURE_H, 16u, VMAF_PIX_FMT_YUV420P};
    return check_case(&c);
}

/* Odd in both dimensions: the last column and row have chroma of their own. */
static char *test_ciede_odd_frame(void)
{
    const Case c = {"ciede 4:2:0 odd", 323u, 181u, 8u, VMAF_PIX_FMT_YUV420P};
    return check_case(&c);
}

static char *test_ciede_422(void)
{
    const Case c = {"ciede 4:2:2", FIXTURE_W, FIXTURE_H, 8u, VMAF_PIX_FMT_YUV422P};
    return check_case(&c);
}

static char *test_ciede_444(void)
{
    const Case c = {"ciede 4:4:4", FIXTURE_W, FIXTURE_H, 10u, VMAF_PIX_FMT_YUV444P};
    return check_case(&c);
}

static char *test_ciede_1080p(void)
{
    const Case c = {"ciede 1080p", 1920u, 1080u, 8u, VMAF_PIX_FMT_YUV420P};
    return check_case(&c);
}

/* 9, 11, 13, 14 and 15 bits (ADR-2145). */
static char *test_ciede_odd_depths(void)
{
    static const unsigned depths[] = {9u, 11u, 13u, 14u, 15u};
    for (size_t k = 0; k < sizeof(depths) / sizeof(depths[0]); k++) {
        const Case c = {"ciede 4:2:0", FIXTURE_W, FIXTURE_H, depths[k], VMAF_PIX_FMT_YUV420P};
        char *const msg = check_case(&c);
        if (msg) {
            return msg;
        }
    }
    return NULL;
}

static char *run_bit_depth_cases(void)
{
    mu_run_test(test_ciede_8bit);
    mu_run_test(test_ciede_10bit);
    mu_run_test(test_ciede_12bit);
    mu_run_test(test_ciede_16bit);
    mu_run_test(test_ciede_odd_depths);
    return NULL;
}

static char *run_layout_cases(void)
{
    mu_run_test(test_ciede_odd_frame);
    mu_run_test(test_ciede_422);
    mu_run_test(test_ciede_444);
    mu_run_test(test_ciede_1080p);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_ciede_cuda_registered);
    mu_assert_msg(run_bit_depth_cases());
    mu_assert_msg(run_layout_cases());
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
