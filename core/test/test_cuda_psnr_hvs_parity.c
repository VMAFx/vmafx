/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * psnr_hvs CPU vs. CUDA: the twin returns the CPU's scores bit for bit
 * (ADR-1397).
 *
 * PSNR-HVS is a perceptual peak-signal-to-noise variant that applies the
 * HVS contrast-sensitivity weighting before the MSE reduction. CPU is in
 * third_party/xiph/psnr_hvs.c (Xiph reference port); CUDA path is in
 * integer_psnr_hvs_cuda.c + integer_psnr_hvs/psnr_hvs_score.cu.
 *
 * calc_psnrhvs() adds every masked coefficient error of a plane into one
 * running float, so its result depends on the order of the additions. The
 * twin stores the same terms and the host adds them in that order; every
 * comparison here is therefore exact, on psnr_hvs_y / psnr_hvs_cb /
 * psnr_hvs_cr and the combined psnr_hvs. The 3840x2160 cases are the ones a
 * twin that sums per block fails by the widest margin: about 10.8 million
 * luma terms, where the CPU's float sum is furthest from the exact one.
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

#define HVS_FEATURES 4u

enum HvsPattern {
    HVS_PATTERN_RAMP,  /* shifted ramps: mostly DC error */
    HVS_PATTERN_MIXED, /* xor-scrambled ramps over the full sample range */
    HVS_PATTERN_NOISE, /* smooth reference, pseudo-random error of a few codes */
};

typedef struct HvsFixture {
    enum VmafPixelFormat fmt;
    unsigned bpc;
    unsigned w;
    unsigned h;
    enum HvsPattern pattern;
} HvsFixture;

static const char *const hvs_features[HVS_FEATURES] = {"psnr_hvs_y", "psnr_hvs_cb", "psnr_hvs_cr",
                                                       "psnr_hvs"};

/* Deterministic 32-bit mix of a sample position (no libc rand). */
static unsigned hvs_hash(unsigned col, unsigned row, unsigned plane)
{
    unsigned h = (col * 0x9E3779B1u) ^ (row * 0x85EBCA77u) ^ (plane * 0xC2B2AE3Du);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

static unsigned hvs_sample(const HvsFixture *fx, unsigned col, unsigned row, unsigned plane,
                           unsigned salt)
{
    const unsigned range = 1u << fx->bpc;
    if (fx->pattern == HVS_PATTERN_RAMP) {
        const unsigned luma = row * 5u + col + salt * 11u;
        const unsigned chroma = row + col * (1u + plane) + salt * 7u;
        return (plane == 0u ? luma : chroma) % range;
    }
    if (fx->pattern == HVS_PATTERN_MIXED)
        return ((col * 37u + row * 11u + plane * 5u) ^ (salt * 91u)) % range;
    /* A slow ramp well inside the range, plus up to +-4 codes on the distorted
     * picture: little masking, so most coefficient errors reach the sum. */
    const unsigned base = range / 4u + ((col + 2u * row + 3u * plane) / 8u) % (range / 2u);
    return salt == 0u ? base : base + hvs_hash(col, row, plane) % 9u - 4u;
}

static void fill_hvs_row(uint8_t *line, unsigned width, const HvsFixture *fx, unsigned row,
                         unsigned plane, unsigned salt)
{
    for (unsigned col = 0; col < width; col++) {
        const unsigned v = hvs_sample(fx, col, row, plane, salt);
        if (fx->bpc > 8u) {
            ((uint16_t *)line)[col] = (uint16_t)v;
        } else {
            line[col] = (uint8_t)v;
        }
    }
}

static int fill_hvs_pic(VmafPicture *pic, const HvsFixture *fx, unsigned salt)
{
    const int err = vmaf_picture_alloc(pic, fx->fmt, fx->bpc, fx->w, fx->h);
    if (err)
        return err;
    const unsigned planes = (fx->fmt == VMAF_PIX_FMT_YUV400P) ? 1u : 3u;
    for (unsigned p = 0; p < planes; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            uint8_t *line = (uint8_t *)pic->data[p] + (size_t)row * (size_t)pic->stride[p];
            fill_hvs_row(line, pic->w[p], fx, row, p, salt);
        }
    }
    return 0;
}

static char *open_hvs(VmafCudaState *cuda_state, VmafContext **vmaf)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    mu_assert("vmaf_init failed", !vmaf_init(vmaf, cfg));
    if (cuda_state)
        mu_assert("vmaf_cuda_import_state failed", !vmaf_cuda_import_state(*vmaf, cuda_state));
    mu_assert("vmaf_use_feature failed",
              !vmaf_use_feature(*vmaf, cuda_state ? "psnr_hvs_cuda" : "psnr_hvs", NULL));
    return NULL;
}

/* Reads the emitted scores of frame 0. A 4:0:0 fixture has no chroma scores;
 * those slots stay 0 on both sides. */
static char *read_hvs_scores(VmafContext *vmaf, const HvsFixture *fx, double scores[HVS_FEATURES])
{
    const int luma_only = (fx->fmt == VMAF_PIX_FMT_YUV400P);
    for (unsigned i = 0; i < HVS_FEATURES; i++) {
        scores[i] = 0.0;
        if (luma_only && (i == 1u || i == 2u))
            continue;
        mu_assert("score", !vmaf_feature_score_at_index(vmaf, hvs_features[i], &scores[i], 0u));
    }
    return NULL;
}

/* Scores of one frame on the CPU (`cuda_state` NULL) or on the twin. */
static char *score_hvs(VmafCudaState *cuda_state, const HvsFixture *fx, double scores[HVS_FEATURES])
{
    VmafContext *vmaf = NULL;
    mu_assert_msg(open_hvs(cuda_state, &vmaf));
    VmafPicture ref;
    VmafPicture dist;
    mu_assert("ref alloc", !fill_hvs_pic(&ref, fx, 0u));
    mu_assert("dist alloc", !fill_hvs_pic(&dist, fx, 1u));
    mu_assert("read", !vmaf_read_pictures(vmaf, &ref, &dist, 0u));
    mu_assert("flush", !vmaf_read_pictures(vmaf, NULL, NULL, 0));
    mu_assert_msg(read_hvs_scores(vmaf, fx, scores));
    mu_assert("vmaf_close failed", !vmaf_close(vmaf));
    return NULL;
}

/* The bit pattern of a score: two scores are the same value, to the last bit
 * and including infinities, exactly when their patterns are equal. */
static uint64_t score_bits(double score)
{
    uint64_t bits = 0u;
    memcpy(&bits, &score, sizeof(bits));
    return bits;
}

/* Bit-for-bit comparison of every score; reports each one that differs. */
static char *require_identical(const HvsFixture *fx, const double cpu[HVS_FEATURES],
                               const double gpu[HVS_FEATURES])
{
    unsigned differing = 0u;
    for (unsigned i = 0; i < HVS_FEATURES; i++) {
        if (score_bits(cpu[i]) == score_bits(gpu[i]))
            continue;
        differing++;
        (void)fprintf(stderr, "\n%ux%u %u-bit %s: cpu=%.17g cuda=%.17g delta=%.3e", fx->w, fx->h,
                      fx->bpc, hvs_features[i], cpu[i], gpu[i], fabs(cpu[i] - gpu[i]));
    }
    if (differing != 0u)
        (void)fprintf(stderr, "\n");
    mu_assert("psnr_hvs_cuda must return the CPU's psnr_hvs scores bit for bit (ADR-1397)",
              differing == 0u);
    return NULL;
}

static char *compare_hvs(const HvsFixture *fx)
{
    VmafCudaState *cuda_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    if (vmaf_cuda_state_init(&cuda_state, cuda_cfg) != 0 || cuda_state == NULL) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        return NULL;
    }
    double cpu[HVS_FEATURES] = {0.0, 0.0, 0.0, 0.0};
    double gpu[HVS_FEATURES] = {NAN, NAN, NAN, NAN};
    char *msg = score_hvs(NULL, fx, cpu);
    if (!msg)
        msg = score_hvs(cuda_state, fx, gpu);
    if (!msg)
        msg = require_identical(fx, cpu, gpu);
    const int free_err = vmaf_cuda_state_free(cuda_state);
    if (!msg && free_err)
        msg = "vmaf_cuda_state_free failed";
    return msg;
}

static char *test_psnr_hvs_cuda_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr_hvs_cuda");
    mu_assert("psnr_hvs_cuda extractor must be registered", fex != NULL);
    mu_assert("psnr_hvs_cuda name matches", !strcmp(fex->name, "psnr_hvs_cuda"));
    return NULL;
}

static char *test_psnr_hvs_cpu_cuda_identical(void)
{
    const HvsFixture ramp = {VMAF_PIX_FMT_YUV420P, 8u, FIXTURE_W, FIXTURE_H, HVS_PATTERN_RAMP};
    mu_assert_msg(compare_hvs(&ramp));
    const HvsFixture noise = {VMAF_PIX_FMT_YUV420P, 8u, FIXTURE_W, FIXTURE_H, HVS_PATTERN_NOISE};
    return compare_hvs(&noise);
}

/* 9- and 11-bit input: calc_psnrhvs() uses the raw sample at every depth. The
 * twin used to convert only 10- and 12-bit samples back exactly and scored
 * 9 / 11 bits on 16 times the sample (-1.57 against 22.47 dB at 9 bits). */
static char *test_psnr_hvs_every_depth_identical(void)
{
    for (unsigned bpc = 9u; bpc <= 12u; bpc++) {
        const HvsFixture fx = {VMAF_PIX_FMT_YUV420P, bpc, 64u, 48u, HVS_PATTERN_MIXED};
        mu_assert_msg(compare_hvs(&fx));
    }
    return NULL;
}

/* 4:0:0 input: the CPU extractor scores luma only; the twin must do the same
 * instead of refusing the format. 4:2:2 and 4:4:4 change the chroma block
 * counts and with them the length of each chroma sum. */
static char *test_psnr_hvs_every_layout_identical(void)
{
    static const enum VmafPixelFormat layouts[] = {VMAF_PIX_FMT_YUV400P, VMAF_PIX_FMT_YUV422P,
                                                   VMAF_PIX_FMT_YUV444P};
    for (unsigned i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
        const HvsFixture fx = {layouts[i], 8u, 64u, 48u, HVS_PATTERN_MIXED};
        mu_assert_msg(compare_hvs(&fx));
    }
    return NULL;
}

/* 3840x2160: 10.8 million luma terms in one running float. A twin that sums
 * each block first is 1e-2 dB away from the CPU here on real content. */
static char *test_psnr_hvs_2160p_identical(void)
{
    const HvsFixture uhd8 = {VMAF_PIX_FMT_YUV420P, 8u, 3840u, 2160u, HVS_PATTERN_NOISE};
    mu_assert_msg(compare_hvs(&uhd8));
    const HvsFixture uhd10 = {VMAF_PIX_FMT_YUV420P, 10u, 3840u, 2160u, HVS_PATTERN_NOISE};
    return compare_hvs(&uhd10);
}

char *run_tests(void)
{
    mu_run_test(test_psnr_hvs_cuda_registered);
    mu_run_test(test_psnr_hvs_cpu_cuda_identical);
    mu_run_test(test_psnr_hvs_every_depth_identical);
    mu_run_test(test_psnr_hvs_every_layout_identical);
    mu_run_test(test_psnr_hvs_2160p_identical);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
