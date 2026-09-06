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
 * The same file also gates the SpEED dense matrix-product kernels
 * (speed_matmul_avx2 / speed_matmul_avx512) against speed_matmul_scalar.
 * Those carry a *stricter* contract than the covariance kernel above:
 * memcmp bit-equality, not a relative tolerance.  The `j` axis they widen
 * is an output index rather than a reduction axis, so vector width cannot
 * change the order in which any single output element accumulates, and
 * both translation units are compiled `-ffp-contract=off` so no FMA
 * fusion collapses the separate multiply and add.  A failure here means
 * one of those two invariants was broken.
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

#include "feature/speed_matmul.h"
#if ARCH_X86
#include "feature/x86/speed_avx2.h"
#include "feature/x86/speed_matmul_avx2.h"
#if HAVE_AVX512
#include "feature/x86/speed_avx512.h"
#include "feature/x86/speed_matmul_avx512.h"
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

/* ---------------------------------------------------------------- */
/* speed_matmul_* — bit-exact (memcmp) parity with speed_matmul_scalar */
/* ---------------------------------------------------------------- */

/* SpEED's own QR matrices are 25x25 (block_size 5 squared); the rectangular
 * solve is 25 x num_blocks.  The shapes below cover the native case plus
 * every tail branch of both kernels (32-wide body, 16/8-wide step, masked
 * and scalar remainders). */
#define MATMUL_FILL_LO (-2.0f)
#define MATMUL_FILL_HI (2.0f)

/* Destinations are file-scope so the memcmp assert (which returns on
 * failure) never leaks a heap buffer.  MATMUL_MAX_D covers the largest
 * shape exercised below, 25 x 448. */
#define MATMUL_MAX_D (25 * 448)
static float matmul_d_scalar[MATMUL_MAX_D];
static float matmul_d_simd[MATMUL_MAX_D];

static char *check_matmul(speed_matmul_fn simd, char *label, uint32_t seed, int rows, int inner,
                          int cols)
{
    const size_t x_elems = (size_t)rows * (size_t)inner;
    const size_t y_elems = (size_t)inner * (size_t)cols;
    const size_t d_elems = (size_t)rows * (size_t)cols;

    if (d_elems > (size_t)MATMUL_MAX_D)
        return "check_matmul: shape exceeds MATMUL_MAX_D";

    float *x = (float *)simd_test_aligned_malloc(x_elems * sizeof(float), 64);
    float *y = (float *)simd_test_aligned_malloc(y_elems * sizeof(float), 64);
    if (!x || !y) {
        simd_test_aligned_free(x);
        simd_test_aligned_free(y);
        return "aligned_malloc failed (matmul)";
    }

    simd_test_fill_random_f32(x, x_elems, MATMUL_FILL_LO, MATMUL_FILL_HI, seed);
    simd_test_fill_random_f32(y, y_elems, MATMUL_FILL_LO, MATMUL_FILL_HI, seed ^ 0x5A5A5A5Au);
    /* Poison both destinations so a kernel that skips lanes is caught. */
    for (size_t i = 0; i < d_elems; i++) {
        matmul_d_scalar[i] = -1.0f;
        matmul_d_simd[i] = -1.0f;
    }

    speed_matmul_scalar(matmul_d_scalar, cols, x, inner, y, cols, rows, inner, cols);
    simd(matmul_d_simd, cols, x, inner, y, cols, rows, inner, cols);

    simd_test_aligned_free(x);
    simd_test_aligned_free(y);

    SIMD_BITEXACT_ASSERT_MEMCMP(matmul_d_scalar, matmul_d_simd, d_elems * sizeof(float), label);
    return NULL;
}

static char *test_matmul_avx2_speed_native(void)
{
    /* 25x25 * 25x25 — the QR-iteration shape (3x8 lanes + 1 scalar). */
    return check_matmul(speed_matmul_avx2, "speed_matmul_avx2 25x25x25", 0xdeadbeefu, 25, 25, 25);
}
static char *test_matmul_avx2_rect(void)
{
    /* 25x25 * 25x448 — the rectangular Q^T B solve (14x32-wide body). */
    return check_matmul(speed_matmul_avx2, "speed_matmul_avx2 25x25x448", 0x12345678u, 25, 25, 448);
}
static char *test_matmul_avx2_tails(void)
{
    /* cols=41 -> one 32-body, one 8-step, one scalar element. */
    return check_matmul(speed_matmul_avx2, "speed_matmul_avx2 7x9x41", 0xabcdef01u, 7, 9, 41);
}
static char *test_matmul_avx2_narrow(void)
{
    /* cols=3 -> scalar-only path; inner=1 -> single accumulation step. */
    return check_matmul(speed_matmul_avx2, "speed_matmul_avx2 4x1x3", 0xfeedfaceu, 4, 1, 3);
}

#if HAVE_AVX512
static char *test_matmul_avx512_speed_native(void)
{
    return check_matmul(speed_matmul_avx512, "speed_matmul_avx512 25x25x25", 0xdeadbeefu, 25, 25,
                        25);
}
static char *test_matmul_avx512_rect(void)
{
    return check_matmul(speed_matmul_avx512, "speed_matmul_avx512 25x25x448", 0x12345678u, 25, 25,
                        448);
}
static char *test_matmul_avx512_tails(void)
{
    /* cols=41 -> one 32-body, one full 16-mask, one 9-lane mask. */
    return check_matmul(speed_matmul_avx512, "speed_matmul_avx512 7x9x41", 0xabcdef01u, 7, 9, 41);
}
static char *test_matmul_avx512_narrow(void)
{
    return check_matmul(speed_matmul_avx512, "speed_matmul_avx512 4x1x3", 0xfeedfaceu, 4, 1, 3);
}
#endif /* HAVE_AVX512 */

#endif /* ARCH_X86 */

char *run_tests(void)
{
#if ARCH_X86
    /* Guarded by `if (have)` rather than an early `return NULL`: the AVX2 and
     * AVX-512 gates then read the same way, and the TU keeps exactly one
     * success exit (ADR-1138 keeps the null pointer constant spelled `NULL`
     * here for the MSVC C lane, so each extra one is measured debt). */
    if (simd_test_have_avx2()) {
        mu_run_test(test_avx2_seed_a);
        mu_run_test(test_avx2_seed_b);
        mu_run_test(test_avx2_aligned_w);
        mu_run_test(test_avx2_tiny);
        mu_run_test(test_matmul_avx2_speed_native);
        mu_run_test(test_matmul_avx2_rect);
        mu_run_test(test_matmul_avx2_tails);
        mu_run_test(test_matmul_avx2_narrow);
#if HAVE_AVX512
        if (simd_test_have_avx512()) {
            mu_run_test(test_avx512_seed_a);
            mu_run_test(test_avx512_seed_b);
            mu_run_test(test_avx512_aligned_w);
            mu_run_test(test_avx512_tiny);
            mu_run_test(test_matmul_avx512_speed_native);
            mu_run_test(test_matmul_avx512_rect);
            mu_run_test(test_matmul_avx512_tails);
            mu_run_test(test_matmul_avx512_narrow);
        }
#endif /* HAVE_AVX512 */
    }
#else
    (void)fprintf(stderr, "skipping: arch lacks Speed covariance SIMD\n");
#endif
    return NULL;
}
