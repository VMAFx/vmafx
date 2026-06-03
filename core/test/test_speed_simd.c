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
 * Numerical-parity contract test for the Speed_chroma covariance-sum SIMD
 * kernels (AVX2 + AVX-512, upstream 30f472b14).
 *
 * Each SIMD kernel (compute_cov_kernel_avx2 / compute_cov_kernel_avx512) is
 * compared against a local scalar reference that mirrors the static
 * compute_cov_kernel_scalar inside speed.c.  The comparison uses a relative
 * tolerance of 1e-9, which is ~5 orders of magnitude tighter than the
 * snapshot gate's places=4 (|abs| < 5e-5).
 *
 * The kernel signature is:
 *   double compute_cov_kernel_*(const float *data_x, const float *data_y,
 *                               size_t stride_px, size_t height, size_t width,
 *                               double mean_x, double mean_y);
 *
 * The kernels use FMA (vfmadd231pd / _mm512_fmadd_pd), which in principle
 * can produce results that differ from the scalar reference by 1 ULP.  In
 * practice the relative residual on all tested fixtures is < 1e-12, well
 * inside the 1e-9 gate.  Per ADR-0138/ADR-0139 the tolerance contract is
 * documented here, not tightened to memcmp.
 *
 * Boilerplate: simd_bitexact_test.h (ADR-0245).
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

#if ARCH_X86
#include "feature/x86/speed_avx2.h"
#if HAVE_AVX512
#include "feature/x86/speed_avx512.h"
#endif
#endif

/* Test dimensions — non-power-of-two to exercise all tail paths. */
#define SPEED_TEST_H 17
#define SPEED_TEST_W 73 /* exercises 8-lane, 4-lane, and scalar tails */

/* Pixel range mirrors SpEED's internal float buffers after picture_copy. */
#define SPEED_FILL_LO (-1.0f)
#define SPEED_FILL_HI (256.0f)

/* Relative tolerance: 1e-9.  Sources of residual:
 *  - FMA contraction fuses (sub * sub) + acc into a single rounding vs.
 *    scalar's two-step multiply-then-add.
 *  - Lane-pair cross-add order in the 8-wide reduction.
 * Both effects are sub-ULP on double, giving rel < 1e-12 in practice.
 * The 1e-9 gate is ~500x tighter than the snapshot gate. */
#define SPEED_COV_REL_TOL 1e-9

/* Scalar reference — mirrors compute_cov_kernel_scalar in speed.c. */
static double scalar_compute_cov(const float *data_x, const float *data_y, size_t stride_px,
                                 size_t height, size_t width, double mean_x, double mean_y)
{
    double result = 0.0;
    for (size_t i = 0; i < height; i++) {
        for (size_t j = 0; j < width; j++) {
            double val_x = data_x[i * stride_px + j];
            double val_y = data_y[i * stride_px + j];
            result += (val_x - mean_x) * (val_y - mean_y);
        }
    }
    return result;
}

#if ARCH_X86

static char *check_avx2(uint32_t seed, int w, int h)
{
    const int stride_px = (w + 7) & ~7; /* round up to 8-float boundary */
    const size_t nelems = (size_t)stride_px * (size_t)h;
    const size_t nbytes = nelems * sizeof(float);

    float *buf_x = (float *)simd_test_aligned_malloc(nbytes, 32);
    float *buf_y = (float *)simd_test_aligned_malloc(nbytes, 32);
    if (!buf_x || !buf_y) {
        simd_test_aligned_free(buf_x);
        simd_test_aligned_free(buf_y);
        return "aligned_malloc failed";
    }

    simd_test_fill_random_f32(buf_x, nelems, SPEED_FILL_LO, SPEED_FILL_HI, seed);
    simd_test_fill_random_f32(buf_y, nelems, SPEED_FILL_LO, SPEED_FILL_HI, seed ^ 0xA5A5A5A5u);

    /* Use mid-range means derived from the seed so they're non-zero. */
    const double mean_x = 64.0 + (double)(seed & 0xFFu) * 0.5;
    const double mean_y = 32.0 + (double)((seed >> 8) & 0xFFu) * 0.5;

    double s_scalar =
        scalar_compute_cov(buf_x, buf_y, (size_t)stride_px, (size_t)h, (size_t)w, mean_x, mean_y);
    double s_avx2 = compute_cov_kernel_avx2(buf_x, buf_y, (size_t)stride_px, (size_t)h, (size_t)w,
                                            mean_x, mean_y);

    simd_test_aligned_free(buf_x);
    simd_test_aligned_free(buf_y);

    SIMD_BITEXACT_ASSERT_RELATIVE(s_scalar, s_avx2, SPEED_COV_REL_TOL,
                                  "compute_cov_kernel_avx2 outside relative tolerance");
    return NULL;
}

static char *test_avx2_seed_a(void)
{
    return check_avx2(0xdeadbeefu, SPEED_TEST_W, SPEED_TEST_H);
}
static char *test_avx2_seed_b(void)
{
    return check_avx2(0x12345678u, SPEED_TEST_W, SPEED_TEST_H);
}
static char *test_avx2_aligned_w(void)
{
    /* w=64 is an exact multiple of 8 — exercises the zero-tail branch. */
    return check_avx2(0xabcdef01u, 64, 16);
}
static char *test_avx2_tiny(void)
{
    /* w=5 exercises 4-lane and scalar-tail paths. */
    return check_avx2(0xfeedface, 5, 3);
}

#if HAVE_AVX512
static char *check_avx512(uint32_t seed, int w, int h)
{
    const int stride_px = (w + 15) & ~15; /* round up to 16-float boundary */
    const size_t nelems = (size_t)stride_px * (size_t)h;
    const size_t nbytes = nelems * sizeof(float);

    float *buf_x = (float *)simd_test_aligned_malloc(nbytes, 64);
    float *buf_y = (float *)simd_test_aligned_malloc(nbytes, 64);
    if (!buf_x || !buf_y) {
        simd_test_aligned_free(buf_x);
        simd_test_aligned_free(buf_y);
        return "aligned_malloc failed (avx512)";
    }

    simd_test_fill_random_f32(buf_x, nelems, SPEED_FILL_LO, SPEED_FILL_HI, seed);
    simd_test_fill_random_f32(buf_y, nelems, SPEED_FILL_LO, SPEED_FILL_HI, seed ^ 0xB6B6B6B6u);

    const double mean_x = 64.0 + (double)(seed & 0xFFu) * 0.5;
    const double mean_y = 32.0 + (double)((seed >> 8) & 0xFFu) * 0.5;

    double s_scalar =
        scalar_compute_cov(buf_x, buf_y, (size_t)stride_px, (size_t)h, (size_t)w, mean_x, mean_y);
    double s_avx512 = compute_cov_kernel_avx512(buf_x, buf_y, (size_t)stride_px, (size_t)h,
                                                (size_t)w, mean_x, mean_y);

    simd_test_aligned_free(buf_x);
    simd_test_aligned_free(buf_y);

    SIMD_BITEXACT_ASSERT_RELATIVE(s_scalar, s_avx512, SPEED_COV_REL_TOL,
                                  "compute_cov_kernel_avx512 outside relative tolerance");
    return NULL;
}

static char *test_avx512_seed_a(void)
{
    return check_avx512(0xdeadbeefu, SPEED_TEST_W, SPEED_TEST_H);
}
static char *test_avx512_seed_b(void)
{
    return check_avx512(0x12345678u, SPEED_TEST_W, SPEED_TEST_H);
}
static char *test_avx512_aligned_w(void)
{
    /* w=64 is an exact multiple of 16 — exercises the zero-tail branch. */
    return check_avx512(0xabcdef01u, 64, 16);
}
static char *test_avx512_tiny(void)
{
    /* w=9 exercises 8-lane fallback and scalar tail paths. */
    return check_avx512(0xfeedface, 9, 3);
}
#endif /* HAVE_AVX512 */

#endif /* ARCH_X86 */

char *run_tests(void)
{
#if ARCH_X86
    if (!simd_test_have_avx2()) {
        return NULL;
    }
    mu_run_test(test_avx2_seed_a);
    mu_run_test(test_avx2_seed_b);
    mu_run_test(test_avx2_aligned_w);
    mu_run_test(test_avx2_tiny);
#if HAVE_AVX512
    if (simd_test_have_avx512()) {
        mu_run_test(test_avx512_seed_a);
        mu_run_test(test_avx512_seed_b);
        mu_run_test(test_avx512_aligned_w);
        mu_run_test(test_avx512_tiny);
    }
#endif /* HAVE_AVX512 */
#else
    (void)fprintf(stderr, "skipping: arch lacks Speed covariance SIMD\n");
#endif
    return NULL;
}
