/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Integer ADM scale-0 contrast masking of an isolated coefficient (ADR-1402;
 * T-ADM-CM-CENTRE-TAP-WRAP-ABOVE-ONE-2026-10-01,
 * T-ADM-CM-X86-TAIL-NEGATIVE-THRESHOLD-SHIFT-2026-10-01,
 * T-ADM-CM-NEGATIVE-THRESHOLD-SHIFT-2026-10-01).
 *
 * The masking threshold sums nine taps per band. Its centre tap, 1/15 of the
 * coefficient, reaches 69904. The fork used to narrow that tap to int16, as
 * the first revision of upstream Netflix/vmaf PR #1602 did, so a coefficient
 * above 15359 wrapped it negative. Where such a coefficient stands alone the
 * eight small neighbours do not bring the sum back above zero, and a negative
 * threshold adds contrast instead of masking it: on a flat reference the
 * scale-0 score came out above 1 where float_adm reports exactly 1. The
 * expression that subtracted the threshold, abs(x) - (thr << shift), was also
 * undefined for a negative thr; the scalar tails of the x86 kernels kept it
 * after the scalar reference stopped using it.
 *
 * The centre tap is int32 now and the excess is clamp(|x| - thr * 2^shift, 0,
 * INT32_MAX), formed in int64. The first four tests pin that clamp in
 * adm_cm_excess_s0(), which the scalar reference and the x86 tails share. The
 * picture tests score a flat reference against the same picture with isolated
 * patches, on the scalar path and on every SIMD level the host has: no
 * integer ADM score may exceed 1, and every level must return the scalar's
 * bits. One picture is wide enough for the vector loops to cover every patch;
 * the other is 24x24 with its patch at (3, 3), which lands in the scalar tail
 * of both x86 kernels. On the sanitizer lane, which halts on the first
 * report, the old tail stops that picture with "left shift of negative value".
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

#define NUM_FRAMES 2u
#define NUM_KEYS 5u

/* The horizontal and vertical bands shift the threshold by 10 bits, the
 * diagonal band by 12 (adm_cm_ctx_init() in integer_adm.c). */
#define SHIFT_HV 10u
#define SHIFT_D 12u

static const char *const SCORE_KEYS[NUM_KEYS] = {
    "integer_adm_scale0",
    "integer_adm_scale1",
    "integer_adm_scale2",
    "integer_adm_scale3",
    "VMAF_integer_feature_adm2_score",
};

/* positive: a threshold below the magnitude is subtracted from it. */
static char *test_excess_subtracts_a_positive_threshold(void)
{
    mu_assert("positive x: 100000 - 3 * 2^10 is 96928",
              adm_cm_excess_s0(100000, 3, SHIFT_HV) == 96928);
    mu_assert("negative x uses its magnitude", adm_cm_excess_s0(-100000, 3, SHIFT_HV) == 96928);
    mu_assert("a shift of 0 subtracts the threshold itself", adm_cm_excess_s0(100, 7, 0u) == 93);
    mu_assert("the diagonal shift of 12 scales the threshold by 4096",
              adm_cm_excess_s0(100000, 3, SHIFT_D) == 87712);
    return NULL;
}

/* negative: a threshold above the magnitude masks the sample completely, and
 * a negative threshold raises the excess. */
static char *test_excess_clamps_at_zero_and_adds_a_negative_threshold(void)
{
    mu_assert("a threshold above |x| masks the sample", adm_cm_excess_s0(100, 1, SHIFT_HV) == 0);
    mu_assert("a threshold equal to |x| masks the sample",
              adm_cm_excess_s0(1024, 1, SHIFT_HV) == 0);
    /* 69904 is the centre tap of a coefficient of -32768; a lone one in each
     * of the three bands gives a threshold of 209712. */
    mu_assert("three full-scale centre taps mask a sample of a million",
              adm_cm_excess_s0(1000000, 3 * 69904, SHIFT_HV) == 0);
    mu_assert("-15176 * 2^10 is -15540224, so the excess is |x| + 15540224",
              adm_cm_excess_s0(1000, -15176, SHIFT_HV) == 15541224);
    mu_assert("-1 * 2^10 is -1024", adm_cm_excess_s0(0, -1, SHIFT_HV) == 1024);
    return NULL;
}

/* boundary: a threshold product that leaves int32 masks the sample; the
 * 32-bit expression wrapped it. */
static char *test_excess_masks_when_the_product_leaves_int32(void)
{
    /* 2^20 * 2^12 is 2^32: modulo 2^32 that is 0 and nothing was masked. */
    mu_assert("a threshold product of 2^32 masks the sample",
              adm_cm_excess_s0(12345, INT32_C(1) << 20, SHIFT_D) == 0);
    /* 2^19 * 2^12 is 2^31: modulo 2^32, 5 - 2^31 wrapped to INT32_MIN + 5. */
    mu_assert("a threshold product of 2^31 masks the sample",
              adm_cm_excess_s0(5, INT32_C(1) << 19, SHIFT_D) == 0);
    mu_assert("the largest threshold masks the largest sample",
              adm_cm_excess_s0(INT32_MAX, INT32_MAX, SHIFT_D) == 0);
    return NULL;
}

/* boundary: an excess above INT32_MAX saturates instead of wrapping. */
static char *test_excess_saturates_at_int32_max(void)
{
    mu_assert("a negative threshold product of -2^31 saturates",
              adm_cm_excess_s0(0, -(INT32_C(1) << 19), SHIFT_D) == INT32_MAX);
    mu_assert("|x| + 1024 above INT32_MAX saturates",
              adm_cm_excess_s0(INT32_MAX, -1, SHIFT_HV) == INT32_MAX);
    mu_assert("the most negative threshold saturates",
              adm_cm_excess_s0(0, INT32_MIN, SHIFT_D) == INT32_MAX);
    mu_assert("the magnitude of INT32_MIN is 2^31, which saturates",
              adm_cm_excess_s0(INT32_MIN, 0, SHIFT_HV) == INT32_MAX);
    mu_assert("INT32_MAX passes through unchanged",
              adm_cm_excess_s0(INT32_MAX, 0, SHIFT_D) == INT32_MAX);
    return NULL;
}

/* A flat grey reference and the same picture with isolated patches. */
typedef struct {
    unsigned w;
    unsigned h;
    unsigned first; /* row and column of the first patch */
    unsigned pitch; /* distance between patches; 0 draws one patch */
} Fixture;

/* Patches every 16 pixels: every one lies inside the x86 vector loops. */
static const Fixture PATCH_GRID = {64u, 64u, 1u, 16u};
/* One patch at (3, 3) of a 24x24 picture: the 12-sample band leaves the
 * vector loops nothing but tail columns. */
static const Fixture PATCH_IN_TAIL = {24u, 24u, 3u, 0u};

static int alloc_grey(VmafPicture *pic, const Fixture *fx)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8u, fx->w, fx->h);
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

/* One white column next to three black ones, two rows high, on mid grey. Each
 * patch gives one large scale-0 coefficient with small neighbours, which is
 * what drove the int16 centre tap, and with it the threshold, below zero.
 * `shift` moves the patches down. */
static void draw_isolated_patches(VmafPicture *pic, const Fixture *fx, unsigned shift)
{
    static const uint8_t PATCH[4] = {255u, 0u, 0u, 0u};
    const unsigned pitch = fx->pitch ? fx->pitch : fx->w + fx->h;
    uint8_t *luma = (uint8_t *)pic->data[0];
    for (unsigned row = fx->first + shift; row + 1u < pic->h[0]; row += pitch) {
        for (unsigned col = fx->first; col + 4u <= pic->w[0]; col += pitch) {
            (void)memcpy(luma + (row * pic->stride[0]) + col, PATCH, sizeof(PATCH));
            (void)memcpy(luma + ((row + 1u) * pic->stride[0]) + col, PATCH, sizeof(PATCH));
        }
    }
}

/* The reference stays flat, so every patch is an additive impairment. The
 * patches move down by two rows in the second frame. */
static int feed_frames(VmafContext *vmaf, const Fixture *fx)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = alloc_grey(&ref, fx);
        if (err) {
            return err;
        }
        err = alloc_grey(&dist, fx);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            return err;
        }
        draw_isolated_patches(&dist, fx, 2u * i);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err) {
            return err;
        }
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

/* `cpumask` 0 lets the extractor dispatch every SIMD level the host has; all
 * bits set forces the scalar path. */
static int run_adm(const Fixture *fx, uint64_t cpumask, double scores[NUM_FRAMES * NUM_KEYS])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .cpumask = cpumask};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (err) {
        return err;
    }
    err = vmaf_use_feature(vmaf, "adm", NULL);
    if (!err) {
        err = feed_frames(vmaf, fx);
    }
    for (unsigned k = 0; !err && k < NUM_FRAMES * NUM_KEYS; k++) {
        err = vmaf_feature_score_at_index(vmaf, SCORE_KEYS[k % NUM_KEYS], &scores[k], k / NUM_KEYS);
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

/* The scalar path, every SIMD level the host has, then AVX2 alone (cpumask
 * bits 16 and 32 turn off AVX-512 and AVX-512 ICL), so an AVX-512 host checks
 * both x86 kernels. */
static const uint64_t CPU_MASKS[] = {~(uint64_t)0, 0u, 16u | 32u};
#define NUM_CPU_MASKS (sizeof(CPU_MASKS) / sizeof(CPU_MASKS[0]))

/* No score of the fixture above 1 on any dispatch level, and every level
 * bit-identical to the scalar path. */
static char *check_fixture(const Fixture *fx)
{
    double scalar[NUM_FRAMES * NUM_KEYS] = {0};
    for (size_t m = 0; m < NUM_CPU_MASKS; m++) {
        double scores[NUM_FRAMES * NUM_KEYS] = {0};
        mu_assert("integer ADM failed on the isolated patches", !run_adm(fx, CPU_MASKS[m], scores));
        if (m == 0u) {
            (void)memcpy(scalar, scores, sizeof(scalar));
        }
        for (unsigned k = 0; k < NUM_FRAMES * NUM_KEYS; k++) {
            if (!(scores[k] >= 0.0 && scores[k] <= 1.0)) {
                (void)fprintf(stderr, "\n  %ux%u cpumask %u frame %u %s: %.17g\n", fx->w, fx->h,
                              (unsigned)CPU_MASKS[m], k / NUM_KEYS, SCORE_KEYS[k % NUM_KEYS],
                              scores[k]);
                return "integer ADM scored an additive impairment outside [0, 1]";
            }
            if (!bit_identical(scores[k], scalar[k])) {
                (void)fprintf(stderr, "\n  %ux%u cpumask %u frame %u %s: %.17g vs scalar %.17g\n",
                              fx->w, fx->h, (unsigned)CPU_MASKS[m], k / NUM_KEYS,
                              SCORE_KEYS[k % NUM_KEYS], scores[k], scalar[k]);
                return "integer ADM SIMD dispatch differs from scalar on an isolated coefficient";
            }
        }
    }
    return NULL;
}

static char *test_isolated_patches_stay_at_or_below_one(void)
{
    return check_fixture(&PATCH_GRID);
}

static char *test_isolated_patch_in_the_simd_tail(void)
{
    return check_fixture(&PATCH_IN_TAIL);
}

char *run_tests(void)
{
    mu_run_test(test_excess_subtracts_a_positive_threshold);
    mu_run_test(test_excess_clamps_at_zero_and_adds_a_negative_threshold);
    mu_run_test(test_excess_masks_when_the_product_leaves_int32);
    mu_run_test(test_excess_saturates_at_int32_max);
    mu_run_test(test_isolated_patches_stay_at_or_below_one);
    mu_run_test(test_isolated_patch_in_the_simd_tail);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
