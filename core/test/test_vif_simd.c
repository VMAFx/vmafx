/**
 *
 *  Copyright 2026 Lusoris
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

/*
 * Numerical-parity contract test for the float-VIF AVX2 statistic kernel
 * (`vif_statistic_s_avx2`).
 *
 * Verifies the ADR-0138 bit-exactness fix: the sigma_max_inv constant must
 * be computed with the same precision as the scalar reference in vif_tools.c.
 * The scalar uses `powf(sigma_nsq, 2.0f) / (255.0 * 255.0)` where the
 * denominator is a double-precision product; the AVX2 path previously used
 * `255.0f * 255.0f` (float), causing 1-5e-6 divergence starting at frame 3
 * of src01_hrc00/hrc01.
 *
 * The test calls both paths on synthetic random inputs and requires the
 * num/den outputs to agree within a tight relative tolerance (1e-7), covering
 * pixel values that exercise the sigma1_sq < sigma_nsq branch (the branch
 * that uses sigma_max_inv).
 *
 * Boilerplate provided by `simd_bitexact_test.h` (ADR-0245).
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "test.h"
/* clang-format off — test.h has no header guard; must precede harness. */
#include "simd_bitexact_test.h"
/* clang-format on */

#include "feature/common/convolution.h"
#include "feature/vif_tools.h"

#if ARCH_X86
#include "feature/x86/vif_statistic_avx2.h"
#include "x86/cpu.h"
#endif

/* Test dimensions: width chosen non-multiple-of-8 to exercise the tail path;
 * height 7 to accumulate across rows. */
#define VIF_TEST_W 37
#define VIF_TEST_H 7
/* Stride in bytes = width * sizeof(float), rounded up to 32-byte alignment. */
#define VIF_STRIDE_PX ((VIF_TEST_W + 7) & ~7)
#define VIF_STRIDE_B ((size_t)VIF_STRIDE_PX * sizeof(float))

/*
 * VIF statistic inputs use plausible image-statistics ranges.
 * mu values in [0, 255], xx/yy/xy_filt (variance-like) in [0, 5000],
 * with some negative xy values to exercise the sigma12 < 0 branch.
 */
#define VIF_MU_LO 0.0f
#define VIF_MU_HI 255.0f
#define VIF_VAR_LO (-500.0f)
#define VIF_VAR_HI 5000.0f

/* Relative tolerance: the fix changes sigma_max_inv by at most one ULP in
 * float32 (~1.2e-7).  Allow 1e-6 to accommodate accumulated row errors. */
#define VIF_REL_TOL 1e-6

#define VIF_AVX512_ALIGN_W 32
#define VIF_AVX512_ALIGN_H 19
#define VIF_AVX512_ALIGN_STRIDE VIF_AVX512_ALIGN_W
#define VIF_AVX512_ALIGN_TOTAL ((size_t)VIF_AVX512_ALIGN_STRIDE * (size_t)VIF_AVX512_ALIGN_H)
#define VIF_AVX512_ALIGN_OFFSET_FLOATS 8
#define VIF_AVX512_ALIGN_EXTRA_FLOATS 16

enum VifAvx512ConvMode {
    VIF_AVX512_CONV_LINEAR,
    VIF_AVX512_CONV_SQUARE,
    VIF_AVX512_CONV_CROSS,
};

struct VifAvx512ConvFixture {
    float *src1_base;
    float *src2_base;
    float *dst_base;
    float *tmp_base;
    float *src1;
    float *src2;
    float *dst;
    float *tmp;
};

static const float vif_avx512_filter[17] = {0.001f, 0.004f, 0.011f, 0.027f, 0.053f, 0.088f,
                                            0.125f, 0.151f, 0.161f, 0.151f, 0.125f, 0.088f,
                                            0.053f, 0.027f, 0.011f, 0.004f, 0.001f};

static float *select_32_aligned_64_misaligned(float *base)
{
    uintptr_t addr = (uintptr_t)base;
    if ((addr % 64u) == 0u) {
        return base + VIF_AVX512_ALIGN_OFFSET_FLOATS;
    }
    return base;
}

static void free_vif_avx512_fixture(struct VifAvx512ConvFixture *fixture)
{
    simd_test_aligned_free(fixture->src1_base);
    simd_test_aligned_free(fixture->src2_base);
    simd_test_aligned_free(fixture->dst_base);
    simd_test_aligned_free(fixture->tmp_base);
}

static char *validate_vif_avx512_alignment(const struct VifAvx512ConvFixture *fixture)
{
    if (((uintptr_t)fixture->src1 % 32u) != 0u) {
        return "src1 must stay 32-byte aligned";
    }
    if (((uintptr_t)fixture->src1 % 64u) == 0u) {
        return "src1 fixture must be 64-byte misaligned";
    }
    if (((uintptr_t)fixture->tmp % 32u) != 0u) {
        return "tmp must stay 32-byte aligned";
    }
    if (((uintptr_t)fixture->tmp % 64u) == 0u) {
        return "tmp fixture must be 64-byte misaligned";
    }
    return NULL;
}

static char *assert_finite_plane(const float *plane)
{
    for (size_t i = 0; i < VIF_AVX512_ALIGN_TOTAL; ++i) {
        if (!isfinite(plane[i])) {
            return "AVX512 convolution produced non-finite output";
        }
    }
    return NULL;
}

#if ARCH_X86

static char *check_vif_stat_avx2(uint32_t seed, int w, int h)
{
    const int stride_px = (w + 7) & ~7;
    const size_t stride_b = (size_t)stride_px * sizeof(float);
    const size_t total = (size_t)stride_px * (size_t)h;

    float *mu1 = (float *)simd_test_aligned_malloc(total * sizeof(float), 32);
    float *mu2 = (float *)simd_test_aligned_malloc(total * sizeof(float), 32);
    float *xx_flt = (float *)simd_test_aligned_malloc(total * sizeof(float), 32);
    float *yy_flt = (float *)simd_test_aligned_malloc(total * sizeof(float), 32);
    float *xy_flt = (float *)simd_test_aligned_malloc(total * sizeof(float), 32);

    if (!mu1 || !mu2 || !xx_flt || !yy_flt || !xy_flt) {
        simd_test_aligned_free(mu1);
        simd_test_aligned_free(mu2);
        simd_test_aligned_free(xx_flt);
        simd_test_aligned_free(yy_flt);
        simd_test_aligned_free(xy_flt);
        return "aligned_malloc failed";
    }

    simd_test_fill_random_f32(mu1, total, VIF_MU_LO, VIF_MU_HI, seed);
    simd_test_fill_random_f32(mu2, total, VIF_MU_LO, VIF_MU_HI, seed ^ 0x11111111u);
    simd_test_fill_random_f32(xx_flt, total, VIF_VAR_LO, VIF_VAR_HI, seed ^ 0x22222222u);
    simd_test_fill_random_f32(yy_flt, total, VIF_VAR_LO, VIF_VAR_HI, seed ^ 0x33333333u);
    simd_test_fill_random_f32(xy_flt, total, VIF_VAR_LO, VIF_VAR_HI, seed ^ 0x44444444u);

    const double sigma_nsq = 2.0; /* default VMAF VIF sigma_nsq */
    const double egl = 100.0;     /* default enhancement gain limit */
    const int stride_b_int = (int)stride_b;

    float num_scalar = 0.0f;
    float den_scalar = 0.0f;
    vif_statistic_s(mu1, mu2, xx_flt, yy_flt, xy_flt, &num_scalar, &den_scalar, w, h, stride_b_int,
                    stride_b_int, stride_b_int, stride_b_int, stride_b_int, egl, sigma_nsq);

    float num_avx2 = 0.0f;
    float den_avx2 = 0.0f;
    vif_statistic_s_avx2(mu1, mu2, xx_flt, yy_flt, xy_flt, &num_avx2, &den_avx2, w, h, stride_b_int,
                         stride_b_int, stride_b_int, stride_b_int, stride_b_int, egl, sigma_nsq);

    simd_test_aligned_free(mu1);
    simd_test_aligned_free(mu2);
    simd_test_aligned_free(xx_flt);
    simd_test_aligned_free(yy_flt);
    simd_test_aligned_free(xy_flt);

    SIMD_BITEXACT_ASSERT_RELATIVE((double)num_scalar, (double)num_avx2, VIF_REL_TOL,
                                  "vif_statistic_s_avx2 num outside tolerance");
    SIMD_BITEXACT_ASSERT_RELATIVE((double)den_scalar, (double)den_avx2, VIF_REL_TOL,
                                  "vif_statistic_s_avx2 den outside tolerance");
    return NULL;
}

static char *test_vif_avx2_seed_a(void)
{
    return check_vif_stat_avx2(0xdeadbeefu, VIF_TEST_W, VIF_TEST_H);
}

static char *test_vif_avx2_seed_b(void)
{
    return check_vif_stat_avx2(0x12345678u, VIF_TEST_W, VIF_TEST_H);
}

static char *test_vif_avx2_aligned_w(void)
{
    /* Width is a multiple of 8 — exercises pure SIMD path with no tail. */
    return check_vif_stat_avx2(0xabcdef01u, 32, VIF_TEST_H);
}

static char *test_vif_avx2_tiny(void)
{
    /* Single row, very small width — exercises tail-only path. */
    return check_vif_stat_avx2(0xfeedface, 3, 1);
}

/* Fixture that maximises coverage of the sigma1_sq < sigma_nsq branch
 * (the branch that directly uses sigma_max_inv, i.e. the fix target).
 * We set xx_flt to small values so sigma1_sq = xx - mu1^2 < sigma_nsq. */
static char *test_vif_avx2_sigma_max_inv_branch(void)
{
    const int w = VIF_TEST_W;
    const int h = VIF_TEST_H;
    const int stride_px = (w + 7) & ~7;
    const size_t total = (size_t)stride_px * (size_t)h;
    const int stride_b_int = stride_px * (int)sizeof(float);

    float *mu1 = (float *)simd_test_aligned_malloc(total * sizeof(float), 32);
    float *mu2 = (float *)simd_test_aligned_malloc(total * sizeof(float), 32);
    float *xx_flt = (float *)simd_test_aligned_malloc(total * sizeof(float), 32);
    float *yy_flt = (float *)simd_test_aligned_malloc(total * sizeof(float), 32);
    float *xy_flt = (float *)simd_test_aligned_malloc(total * sizeof(float), 32);

    if (!mu1 || !mu2 || !xx_flt || !yy_flt || !xy_flt) {
        simd_test_aligned_free(mu1);
        simd_test_aligned_free(mu2);
        simd_test_aligned_free(xx_flt);
        simd_test_aligned_free(yy_flt);
        simd_test_aligned_free(xy_flt);
        return "aligned_malloc failed";
    }

    /* Fill mu with large values so mu^2 ≫ xx_flt → sigma1_sq < 0 → clamped
     * to 0 < sigma_nsq (2.0), exercising the sigma_max_inv branch. */
    simd_test_fill_random_f32(mu1, total, 100.0f, 200.0f, 0xaaaabbbb);
    simd_test_fill_random_f32(mu2, total, 100.0f, 200.0f, 0xbbbbcccc);
    simd_test_fill_random_f32(xx_flt, total, 0.0f, 1.0f, 0xccccdddd);
    simd_test_fill_random_f32(yy_flt, total, 0.0f, 5000.0f, 0xddddeeee);
    simd_test_fill_random_f32(xy_flt, total, -500.0f, 500.0f, 0xeeeeffff);

    const double sigma_nsq = 2.0;
    const double egl = 100.0;

    float num_scalar = 0.0f;
    float den_scalar = 0.0f;
    vif_statistic_s(mu1, mu2, xx_flt, yy_flt, xy_flt, &num_scalar, &den_scalar, w, h, stride_b_int,
                    stride_b_int, stride_b_int, stride_b_int, stride_b_int, egl, sigma_nsq);

    float num_avx2 = 0.0f;
    float den_avx2 = 0.0f;
    vif_statistic_s_avx2(mu1, mu2, xx_flt, yy_flt, xy_flt, &num_avx2, &den_avx2, w, h, stride_b_int,
                         stride_b_int, stride_b_int, stride_b_int, stride_b_int, egl, sigma_nsq);

    simd_test_aligned_free(mu1);
    simd_test_aligned_free(mu2);
    simd_test_aligned_free(xx_flt);
    simd_test_aligned_free(yy_flt);
    simd_test_aligned_free(xy_flt);

    SIMD_BITEXACT_ASSERT_RELATIVE((double)num_scalar, (double)num_avx2, VIF_REL_TOL,
                                  "vif_statistic_s_avx2 sigma_max_inv branch num");
    SIMD_BITEXACT_ASSERT_RELATIVE((double)den_scalar, (double)den_avx2, VIF_REL_TOL,
                                  "vif_statistic_s_avx2 sigma_max_inv branch den");
    return NULL;
}

#if HAVE_AVX512
static char *init_vif_avx512_fixture(struct VifAvx512ConvFixture *fixture)
{
    const size_t elems = VIF_AVX512_ALIGN_TOTAL + VIF_AVX512_ALIGN_EXTRA_FLOATS;
    const size_t bytes = elems * sizeof(float);

    fixture->src1_base = (float *)simd_test_aligned_malloc(bytes, 32);
    fixture->src2_base = (float *)simd_test_aligned_malloc(bytes, 32);
    fixture->dst_base = (float *)simd_test_aligned_malloc(bytes, 32);
    fixture->tmp_base = (float *)simd_test_aligned_malloc(bytes, 32);

    if (!fixture->src1_base || !fixture->src2_base || !fixture->dst_base || !fixture->tmp_base) {
        return "aligned_malloc failed";
    }

    fixture->src1 = select_32_aligned_64_misaligned(fixture->src1_base);
    fixture->src2 = select_32_aligned_64_misaligned(fixture->src2_base);
    fixture->dst = select_32_aligned_64_misaligned(fixture->dst_base);
    fixture->tmp = select_32_aligned_64_misaligned(fixture->tmp_base);

    char *result = validate_vif_avx512_alignment(fixture);
    if (result != NULL) {
        return result;
    }

    simd_test_fill_random_f32(fixture->src1, VIF_AVX512_ALIGN_TOTAL, 0.0f, 255.0f, 0x510dfaceu);
    simd_test_fill_random_f32(fixture->src2, VIF_AVX512_ALIGN_TOTAL, 0.0f, 255.0f, 0x0ddba11u);
    return NULL;
}

static char *run_vif_avx512_convolution(struct VifAvx512ConvFixture *fixture,
                                        enum VifAvx512ConvMode mode)
{
    switch (mode) {
    case VIF_AVX512_CONV_LINEAR:
        convolution_f32_avx512_s(vif_avx512_filter, 17, fixture->src1, fixture->dst, fixture->tmp,
                                 VIF_AVX512_ALIGN_W, VIF_AVX512_ALIGN_H, VIF_AVX512_ALIGN_STRIDE,
                                 VIF_AVX512_ALIGN_STRIDE);
        break;
    case VIF_AVX512_CONV_SQUARE:
        convolution_f32_avx512_sq_s(vif_avx512_filter, 17, fixture->src1, fixture->dst,
                                    fixture->tmp, VIF_AVX512_ALIGN_W, VIF_AVX512_ALIGN_H,
                                    VIF_AVX512_ALIGN_STRIDE, VIF_AVX512_ALIGN_STRIDE);
        break;
    case VIF_AVX512_CONV_CROSS:
        convolution_f32_avx512_xy_s(vif_avx512_filter, 17, fixture->src1, fixture->src2,
                                    fixture->dst, fixture->tmp, VIF_AVX512_ALIGN_W,
                                    VIF_AVX512_ALIGN_H, VIF_AVX512_ALIGN_STRIDE,
                                    VIF_AVX512_ALIGN_STRIDE, VIF_AVX512_ALIGN_STRIDE);
        break;
    default:
        return "unknown AVX512 convolution mode";
    }
    return NULL;
}

static char *check_vif_avx512_32byte_alignment(enum VifAvx512ConvMode mode)
{
    struct VifAvx512ConvFixture fixture = {0};
    char *result = init_vif_avx512_fixture(&fixture);
    if (result == NULL) {
        result = run_vif_avx512_convolution(&fixture, mode);
    }
    if (result == NULL) {
        result = assert_finite_plane(fixture.dst);
    }
    free_vif_avx512_fixture(&fixture);
    return result;
}

static char *test_vif_avx512_convolution_32byte_alignment(void)
{
    char *result = check_vif_avx512_32byte_alignment(VIF_AVX512_CONV_LINEAR);
    if (result == NULL) {
        result = check_vif_avx512_32byte_alignment(VIF_AVX512_CONV_SQUARE);
    }
    if (result == NULL) {
        result = check_vif_avx512_32byte_alignment(VIF_AVX512_CONV_CROSS);
    }
    return result;
}
#endif /* HAVE_AVX512 */

#endif /* ARCH_X86 */

char *run_tests(void)
{
#if ARCH_X86
    if (!simd_test_have_avx2()) {
        return NULL;
    }
    mu_run_test(test_vif_avx2_seed_a);
    mu_run_test(test_vif_avx2_seed_b);
    mu_run_test(test_vif_avx2_aligned_w);
    mu_run_test(test_vif_avx2_tiny);
    mu_run_test(test_vif_avx2_sigma_max_inv_branch);
#if HAVE_AVX512
    if (vmaf_get_cpu_flags_x86() & VMAF_X86_CPU_FLAG_AVX512) {
        mu_run_test(test_vif_avx512_convolution_32byte_alignment);
    } else {
        (void)fprintf(stderr, "skipping: CPU lacks AVX512\n");
    }
#endif
#else
    (void)fprintf(stderr, "skipping: non-x86 arch\n");
#endif
    return NULL;
}
