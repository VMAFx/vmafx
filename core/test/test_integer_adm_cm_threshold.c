/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Integer ADM scale-0 contrast masking with a negative threshold
 * (T-ADM-CM-NEGATIVE-THRESHOLD-SHIFT-2026-10-01).
 *
 * The masking threshold sums nine taps per band. Its centre tap is narrowed
 * to int16, so a coefficient above about 13800 wraps it negative, and when
 * that coefficient stands alone the eight small neighbours do not bring the
 * sum back above zero. The scalar reference then evaluated
 * abs(x) - (thr << shift) with a negative thr, which C leaves undefined; the
 * AVX2 and AVX-512 kernels compute the same expression modulo 2^32.
 *
 * The first three tests pin the modular arithmetic of adm_cm_excess_s0(),
 * which the scalar reference now calls. The last one scores a picture that
 * reaches a negative threshold, on the scalar path and on every SIMD level
 * the host has, and requires identical results. On the sanitizer lane, which
 * halts on the first report, the old scalar expression stops that test with
 * "left shift of negative value -15176". The picture is wide enough for the
 * x86 vector loops to cover every patch; their scalar tails keep upstream's
 * expression (T-ADM-CM-X86-TAIL-NEGATIVE-THRESHOLD-SHIFT-2026-10-01).
 *
 * The scores themselves are not pinned. With a flat reference the negative
 * threshold turns into masked contrast that is not in the picture, so scale 0
 * comes out above 1 where float_adm reports exactly 1; that follows from the
 * int16 centre tap, not from the shift, and this test must not freeze it.
 */

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/adm_cm_accumulator.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#define FRAME_W 64u
#define FRAME_H 64u
#define NUM_FRAMES 2u
#define NUM_SCALES 4u

/* The horizontal and vertical bands shift the threshold by 10 bits, the
 * diagonal band by 12 (adm_cm_ctx_init() in integer_adm.c). */
#define SHIFT_HV 10u
#define SHIFT_D 12u

static const char *const SCALE_KEYS[NUM_SCALES] = {
    "integer_adm_scale0",
    "integer_adm_scale1",
    "integer_adm_scale2",
    "integer_adm_scale3",
};

/* positive: an ordinary threshold is subtracted from the magnitude of x. */
static char *test_excess_subtracts_a_positive_threshold(void)
{
    mu_assert("positive x: 100000 - (3 << 10) is 96928",
              adm_cm_excess_s0(100000, 3, SHIFT_HV) == 96928);
    mu_assert("negative x uses its magnitude", adm_cm_excess_s0(-100000, 3, SHIFT_HV) == 96928);
    mu_assert("a threshold above |x| gives a negative excess for the caller to clamp",
              adm_cm_excess_s0(100, 1, SHIFT_HV) == -924);
    mu_assert("a shift of 0 subtracts the threshold itself", adm_cm_excess_s0(100, 7, 0u) == 93);
    return NULL;
}

/* negative: a negative threshold raises the excess, as the SIMD kernels do. */
static char *test_excess_adds_a_negative_threshold(void)
{
    /* -15176 is the threshold the picture below produces. */
    mu_assert("-15176 << 10 is -15540224, so the excess is |x| + 15540224",
              adm_cm_excess_s0(1000, -15176, SHIFT_HV) == 15541224);
    mu_assert("the diagonal shift of 12 scales the same threshold by 4096",
              adm_cm_excess_s0(-1000, -15176, SHIFT_D) == 62161896);
    mu_assert("-1 << 10 is -1024", adm_cm_excess_s0(0, -1, SHIFT_HV) == 1024);
    return NULL;
}

/* boundary: operands at the edge of int32 wrap modulo 2^32 instead of
 * overflowing. */
static char *test_excess_wraps_modulo_two_to_the_32(void)
{
    /* 2^20 << 12 is 2^32, which is 0 modulo 2^32. */
    mu_assert("a threshold that shifts out of 32 bits wraps to 0",
              adm_cm_excess_s0(12345, INT32_C(1) << 20, SHIFT_D) == 12345);
    /* 2^19 << 12 is 2^31; 5 - 2^31 wraps to INT32_MIN + 5. */
    mu_assert("a threshold that shifts into the sign bit wraps",
              adm_cm_excess_s0(5, INT32_C(1) << 19, SHIFT_D) == INT32_MIN + 5);
    mu_assert("the magnitude of INT32_MIN is 2^31, not an overflow",
              adm_cm_excess_s0(INT32_MIN, 0, SHIFT_HV) == INT32_MIN);
    mu_assert("INT32_MAX passes through unchanged",
              adm_cm_excess_s0(INT32_MAX, 0, SHIFT_D) == INT32_MAX);
    return NULL;
}

static int alloc_grey(VmafPicture *pic)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8u, FRAME_W, FRAME_H);
    if (err) {
        return err;
    }
    for (unsigned p = 0; p < 3u; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            (void)memset(plane + (row * pic->stride[p]), 128, pic->w[p]);
        }
    }
    return 0;
}

/* One white column next to three black ones, two rows high, every 16 pixels
 * on mid grey. Each patch gives one large scale-0 coefficient with small
 * neighbours, which is what drives the masking threshold below zero. */
static void draw_isolated_patches(VmafPicture *pic, unsigned shift)
{
    static const uint8_t PATCH[4] = {255u, 0u, 0u, 0u};
    uint8_t *luma = (uint8_t *)pic->data[0];
    for (unsigned row = 1u + shift; row + 1u < pic->h[0]; row += 16u) {
        for (unsigned col = 1u; col + 4u <= pic->w[0]; col += 16u) {
            (void)memcpy(luma + (row * pic->stride[0]) + col, PATCH, sizeof(PATCH));
            (void)memcpy(luma + ((row + 1u) * pic->stride[0]) + col, PATCH, sizeof(PATCH));
        }
    }
}

/* The reference stays flat, so every patch is an additive impairment. The
 * patches move down by two rows in the second frame. */
static int feed_frames(VmafContext *vmaf)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = alloc_grey(&ref);
        if (err) {
            return err;
        }
        err = alloc_grey(&dist);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            return err;
        }
        draw_isolated_patches(&dist, 2u * i);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err) {
            return err;
        }
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

/* `cpumask` 0 lets the extractor dispatch every SIMD level the host has; all
 * bits set forces the scalar path. */
static int run_adm(uint64_t cpumask, double scores[NUM_FRAMES * NUM_SCALES])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .cpumask = cpumask};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (err) {
        return err;
    }
    err = vmaf_use_feature(vmaf, "adm", NULL);
    if (!err) {
        err = feed_frames(vmaf);
    }
    for (unsigned k = 0; !err && k < NUM_FRAMES * NUM_SCALES; k++) {
        err = vmaf_feature_score_at_index(vmaf, SCALE_KEYS[k % NUM_SCALES], &scores[k],
                                          k / NUM_SCALES);
    }
    (void)vmaf_close(vmaf);
    return err;
}

/* Bit-pattern equality; memcmp() over floating-point operands is what
 * clang-tidy rightly flags, so compare the payloads as integers. */
static int bit_identical(double a, double b)
{
    uint64_t a_bits = 0;
    uint64_t b_bits = 0;
    (void)memcpy(&a_bits, &a, sizeof(a_bits));
    (void)memcpy(&b_bits, &b, sizeof(b_bits));
    return a_bits == b_bits;
}

/* Every SIMD level the host has, then AVX2 alone (cpumask bits 16 and 32 turn
 * off AVX-512 and AVX-512 ICL), so an AVX-512 host checks both x86 kernels. */
static const uint64_t SIMD_MASKS[] = {0u, 16u | 32u};
#define NUM_SIMD_MASKS (sizeof(SIMD_MASKS) / sizeof(SIMD_MASKS[0]))

static char *test_negative_threshold_scores_the_same_on_every_dispatch(void)
{
    double scalar[NUM_FRAMES * NUM_SCALES] = {0};
    mu_assert("integer ADM failed on the isolated patches (scalar)",
              !run_adm(~(uint64_t)0, scalar));
    for (unsigned k = 0; k < NUM_FRAMES * NUM_SCALES; k++) {
        mu_assert("integer ADM scored the isolated patches as a non-finite value",
                  isfinite(scalar[k]));
    }

    for (size_t m = 0; m < NUM_SIMD_MASKS; m++) {
        double simd[NUM_FRAMES * NUM_SCALES] = {0};
        mu_assert("integer ADM failed on the isolated patches (SIMD)",
                  !run_adm(SIMD_MASKS[m], simd));
        for (unsigned k = 0; k < NUM_FRAMES * NUM_SCALES; k++) {
            if (!bit_identical(simd[k], scalar[k])) {
                (void)fprintf(stderr, "\n  cpumask %u frame %u %s: SIMD %.17g vs scalar %.17g\n",
                              (unsigned)SIMD_MASKS[m], k / NUM_SCALES, SCALE_KEYS[k % NUM_SCALES],
                              simd[k], scalar[k]);
                return "integer ADM SIMD dispatch differs from scalar on a negative threshold";
            }
        }
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_excess_subtracts_a_positive_threshold);
    mu_run_test(test_excess_adds_a_negative_threshold);
    mu_run_test(test_excess_wraps_modulo_two_to_the_32);
    mu_run_test(test_negative_threshold_scores_the_same_on_every_dispatch);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
