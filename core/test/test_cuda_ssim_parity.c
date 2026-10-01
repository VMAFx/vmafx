/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * integer ssim CPU vs. CUDA parity (round-2 GPU-kernel coverage; exact since
 * ADR-1424).
 *
 * SSIM is integer_ssim.c on the CPU (registered as "ssim") and ssim_cuda.c
 * plus integer_ssim/integer_ssim_score.cu on CUDA (registered as
 * "integer_ssim_cuda"); both emit the feature "ssim". The moments are int64
 * on both sides and the per-pixel term is one double expression, so the only
 * thing that can differ is the frame sum: calc_ssim() adds every term into
 * one double in raster order. Since ADR-1424 the kernel leaves the terms
 * unreduced and the host adds the plane it reads back in that order, so this
 * test asserts equality, not a tolerance.
 *
 * Before ADR-1424 the kernel reduced each warp and each 16x8 block and the
 * host added the blocks, which put the score a few units in the last place
 * from the CPU's (1e-11 at most on the fixtures, 3.6e-10 in dB). Every case
 * below with more than one pixel per block fails on that twin.
 *
 * Skip behaviour: skips with "[skip: no CUDA device]" when no CUDA driver.
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

/* One case: a frame geometry and an optional boolean option. */
typedef struct Case {
    const char *what;
    unsigned w;
    unsigned h;
    unsigned bpc;
    const char *option; /* NULL for the defaults */
} Case;

/* Deterministic position hash. */
static unsigned position_hash(unsigned row, unsigned col, unsigned salt)
{
    unsigned x = row * 73856093u ^ col * 19349663u ^ salt * 83492791u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return x;
}

/* Luma in 8-bit levels: structure that varies from window to window, and a
 * distorted frame with a position-dependent error, so the per-pixel terms
 * span a range of magnitudes and their sum depends on its order. */
static unsigned fixture_luma(unsigned row, unsigned col, bool distorted)
{
    const unsigned base = 48u + (position_hash(row >> 2, col >> 2, 1u) % 160u);
    if (!distorted)
        return base;
    return base + (position_hash(row, col, 2u) % 23u);
}

static void put_sample(VmafPicture *pic, unsigned plane, unsigned row, unsigned col, unsigned v)
{
    const unsigned peak = (1u << pic->bpc) - 1u;
    uint8_t *line = (uint8_t *)pic->data[plane] + (size_t)row * (size_t)pic->stride[plane];
    if (pic->bpc <= 8u) {
        line[col] = (uint8_t)(v > peak ? peak : v);
    } else {
        ((uint16_t *)line)[col] = (uint16_t)(v > peak ? peak : v);
    }
}

static int fill_picture(VmafPicture *pic, const Case *c, bool distorted)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, c->bpc, c->w, c->h);
    if (err)
        return err;
    const unsigned gain = 1u << (c->bpc - 8u);
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++)
            put_sample(pic, 0u, row, col, fixture_luma(row, col, distorted) * gain);
    }
    for (unsigned p = 1; p < 3; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++)
                put_sample(pic, p, row, col, 128u * gain);
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

static VmafFeatureDictionary *case_opts(const Case *c)
{
    VmafFeatureDictionary *d = NULL;
    if (c->option && vmaf_feature_dictionary_set(&d, c->option, "true"))
        return NULL;
    return d;
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
              !vmaf_use_feature(vmaf, cu_state ? "integer_ssim_cuda" : "ssim", case_opts(c)));
    char *msg = feed_one_frame(vmaf, c);
    if (msg)
        return msg;
    mu_assert("vmaf_feature_score_at_index(ssim) failed",
              !vmaf_feature_score_at_index(vmaf, "ssim", score, 0u));
    mu_assert("vmaf_close failed", !vmaf_close(vmaf));
    return NULL;
}

/* The CUDA twin's score equals the CPU's, bit for bit. A missing device
 * skips the case. */
static char *check_exact(const Case *c)
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

    mu_assert("CPU ssim is NaN", !isnan(cpu));
    if (cpu != gpu) {
        (void)fprintf(stderr, "\n%s %ux%u %u-bit: cpu=%.17g cuda=%.17g delta=%.3e\n", c->what, c->w,
                      c->h, c->bpc, cpu, gpu, fabs(cpu - gpu));
    }
    mu_assert("integer_ssim_cuda differs from the CPU extractor", cpu == gpu);
    return NULL;
}

static char *test_ssim_cuda_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("integer_ssim_cuda");
    mu_assert("integer_ssim_cuda extractor must be registered", fex != NULL);
    mu_assert("integer_ssim_cuda name matches", !strcmp(fex->name, "integer_ssim_cuda"));
    return NULL;
}

static char *test_ssim_default_exact(void)
{
    const Case c = {"ssim", FIXTURE_W, FIXTURE_H, 8u, NULL};
    return check_exact(&c);
}

static char *test_ssim_10bit_exact(void)
{
    const Case c = {"ssim", FIXTURE_W, FIXTURE_H, 10u, NULL};
    return check_exact(&c);
}

static char *test_ssim_12bit_exact(void)
{
    const Case c = {"ssim", FIXTURE_W, FIXTURE_H, 12u, NULL};
    return check_exact(&c);
}

/* samplemax^2 exceeds INT_MAX here; the constants are formed in double on
 * both sides. */
static char *test_ssim_16bit_exact(void)
{
    const Case c = {"ssim", FIXTURE_W, FIXTURE_H, 16u, NULL};
    return check_exact(&c);
}

/* Neither dimension is a multiple of the 16x8 block, so the last column and
 * row of blocks are partial. */
static char *test_ssim_odd_frame_exact(void)
{
    const Case c = {"ssim odd", 323u, 181u, 8u, NULL};
    return check_exact(&c);
}

/* Narrower than the nine-tap window: every pixel's window is truncated and
 * its weight is not a power of two. */
static char *test_ssim_tiny_frame_exact(void)
{
    const Case c = {"ssim tiny", 7u, 5u, 8u, NULL};
    return check_exact(&c);
}

static char *test_ssim_1080p_exact(void)
{
    const Case c = {"ssim 1080p", 1920u, 1080u, 8u, NULL};
    return check_exact(&c);
}

/* -10 * log10(1 - ssim) magnifies a last-place difference of the ratio. */
static char *test_ssim_enable_db_exact(void)
{
    const Case c = {"ssim enable_db", FIXTURE_W, FIXTURE_H, 8u, "enable_db"};
    return check_exact(&c);
}

static char *run_bit_depth_cases(void)
{
    mu_run_test(test_ssim_default_exact);
    mu_run_test(test_ssim_10bit_exact);
    mu_run_test(test_ssim_12bit_exact);
    mu_run_test(test_ssim_16bit_exact);
    return NULL;
}

static char *run_geometry_and_option_cases(void)
{
    mu_run_test(test_ssim_odd_frame_exact);
    mu_run_test(test_ssim_tiny_frame_exact);
    mu_run_test(test_ssim_1080p_exact);
    mu_run_test(test_ssim_enable_db_exact);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_ssim_cuda_registered);
    mu_assert_msg(run_bit_depth_cases());
    mu_assert_msg(run_geometry_and_option_cases());
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
