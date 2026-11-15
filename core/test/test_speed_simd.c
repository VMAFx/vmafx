/**
 *
 *  Copyright 2016-2025 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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
 * Bit-exactness contract test for SpEED's SIMD kernels.
 *
 * Covariance row kernels (speed_cov_row_avx2 / speed_cov_row_avx512 /
 * speed_cov_row_neon, ADR-1459): every sum a row kernel returns is compared,
 * as eight bytes, with compute_cov_kernel_scalar() of speed.c for the same
 * pair of blocks. The reference is the production function, not a copy: a
 * copy compiled in this file could be contracted differently from the one the
 * extractor runs. A lane is one covariance sum, so vector width cannot change
 * the order in which a sum accumulates, and the kernels multiply and add
 * separately; a failure here means one of those two properties was broken
 * (an FMA, or a sum split over lanes, as in the kernels of upstream 30f472b14
 * and 15297286 that this fork does not dispatch).
 *
 * The matrix: the block sizes of upstream's checkasm case (Netflix/vmaf
 * 15297286, 1x1 to 256x64) and the block shapes SpEED meets from 576x324 to
 * 3840x2160; nine input patterns from picture-range noise to signed zeros,
 * subnormals, cancelling 1e30 terms and products that overflow; tight, padded
 * and unaligned planes; the fixed means of upstream's case and each block's
 * own; every count from 1 to SPEED_COV_ROW_MAX. Each plane is allocated at
 * exactly the size the contract lets a kernel read, so a sanitizer build sees
 * any read past it, and the sums past `count` must stay untouched.
 *
 * The same file also gates the SpEED dense matrix-product kernels
 * (speed_matmul_avx2 / speed_matmul_avx512) against speed_matmul_scalar,
 * again with memcmp.  The `j` axis they widen
 * is an output index rather than a reduction axis, so vector width cannot
 * change the order in which any single output element accumulates, and
 * both translation units are compiled `-ffp-contract=off` so no FMA
 * fusion collapses the separate multiply and add.  A failure here means
 * one of those two invariants was broken.
 */

#include <math.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "mu_table.h"
#include "test.h"
/* clang-format off — test.h has no header guard; must precede harness. */
#include "simd_bitexact_test.h"
/* clang-format on */

#include "feature/speed_cov.h"
#include "feature/speed_matmul.h"
#if ARCH_X86
#include "feature/x86/speed_avx2.h"
#include "feature/x86/speed_matmul_avx2.h"
#if HAVE_AVX512
#include "feature/x86/speed_avx512.h"
#include "feature/x86/speed_matmul_avx512.h"
#endif
#endif
#if ARCH_AARCH64
#include "feature/arm64/speed_neon.h"
#endif

/* ---------------------------------------------------------------- */
/* speed_cov_row_* — bit-exact parity with compute_cov_kernel_scalar */
/* ---------------------------------------------------------------- */

#define COV_PATTERNS 9u
#define COV_LAYOUTS 3u
#define COV_SUMS 8
#define COV_GUARD (-7.0)

typedef struct CovSize {
    size_t w;
    size_t h;
} CovSize;

typedef struct CovPlanes {
    float *x_base;
    float *y_base;
    float *x;
    float *y;
    size_t stride;
} CovPlanes;

static const CovSize cov_sizes[] = {
    /* Netflix/vmaf 15297286, checkasm check_compute_cov_kernel() */
    {1, 1},
    {2, 3},
    {3, 2},
    {4, 4},
    {5, 3},
    {6, 2},
    {7, 3},
    {8, 2},
    {9, 3},
    {15, 7},
    {16, 16},
    {17, 5},
    {37, 9},
    {255, 63},
    {256, 64},
    /* SpEED's blocks: 576x324 chroma and luma, 1080p chroma, 1080p luma */
    {11, 6},
    {31, 16},
    {56, 26},
    {116, 61},
};

static float cov_uniform(uint32_t *state)
{
    return (float)(simd_test_xorshift32(state) >> 8u) / (float)0x01000000;
}

/* Patterns 0..4: values a picture plane or a filtered plane holds. */
static float cov_sample_plain(unsigned pattern, uint32_t *state, size_t i)
{
    switch (pattern) {
    case 0: /* picture range */
        return cov_uniform(state) * 257.0f - 1.0f;
    case 1: /* every sample equals the fixed mean of x: all products are 0 */
        return 127.5f;
    case 2: /* a small step either side of the mean */
        return 127.5f + ((i & 1u) ? 0.125f : -0.125f);
    case 3: /* signed, like the mean-subtracted plane SpEED feeds in */
        return (float)((int)(simd_test_xorshift32(state) & 65535u) - 32768) / 128.0f;
    default: /* checkerboard-like extremes */
        return (simd_test_xorshift32(state) & 1u) ? 255.0f : 0.0f;
    }
}

/* Patterns 5..8: where a different rounding or lane order shows first. */
static float cov_sample_edge(unsigned pattern, uint32_t *state)
{
    const uint32_t r = simd_test_xorshift32(state);
    switch (pattern) {
    case 5: /* a wide range of exponents */
        return ldexpf((float)(r & 0xffffffu), (int)((r >> 24u) % 80u) - 60);
    case 6: /* mostly zero, a few huge terms of either sign: cancellation */
        if ((r & 3u) != 0u)
            return 0.0f;
        return (r & 4u) ? 1.0e30f : -1.0e30f;
    case 7: /* signed zeros and subnormals */
        if ((r & 3u) == 0u)
            return (r & 4u) ? -0.0f : 0.0f;
        return (float)((r >> 3u) % 3u) * 1.4e-45f;
    default: /* products overflow: the sums reach an infinity or a NaN */
        return (float)((int)(r % 2001u) - 1000) * 3.0e36f;
    }
}

static void cov_fill(float *buf, size_t n, unsigned pattern, uint32_t seed)
{
    uint32_t state = seed;
    for (size_t i = 0; i < n; i++) {
        buf[i] =
            pattern < 5u ? cov_sample_plain(pattern, &state, i) : cov_sample_edge(pattern, &state);
    }
}

/* layout 0: stride == readable extent. 1: three floats of padding per row.
 * 2: padded, and both planes one and three floats off the allocation. Each
 * plane ends with the last float the contract lets a kernel read. */
static int cov_planes_alloc(CovPlanes *p, CovSize size, unsigned layout)
{
    const size_t readable = size.w + SPEED_COV_ROW_MAX - 1;
    const size_t off_x = layout == 2u ? 1u : 0u;
    const size_t off_y = layout == 2u ? 3u : 0u;
    p->stride = readable + (layout == 0u ? 0u : 3u);
    const size_t n = (size.h - 1) * p->stride + readable;
    p->x_base = malloc((n + off_x) * sizeof(float));
    p->y_base = malloc((n + off_y) * sizeof(float));
    if (!p->x_base || !p->y_base)
        return 0;
    p->x = p->x_base + off_x;
    p->y = p->y_base + off_y;
    return 1;
}

static void cov_planes_free(CovPlanes *p)
{
    free(p->x_base);
    free(p->y_base);
}

/* The mean compute_mean() of speed.c hands the kernel: a float. */
static double cov_block_mean(const float *block, size_t stride, CovSize size)
{
    float sum = 0.0f;
    for (size_t i = 0; i < size.h; i++) {
        for (size_t j = 0; j < size.w; j++)
            sum += block[i * stride + j];
    }
    const float mean = sum / (float)(size.w * size.h);
    return (double)mean;
}

/* The object representation of a double: -0.0 and +0.0 differ, and so do two
 * NaNs with different payloads, which is what "the same bits" means here. */
static uint64_t cov_bits(double value)
{
    uint64_t bits;
    (void)memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* One x block against `count` y blocks: 0 when every sum has the reference's
 * bits and nothing past `count` was written. */
static int cov_row_mismatches(speed_cov_row_fn kernel, const CovPlanes *p, CovSize size,
                              double mean_x, const double *mean_y, size_t count)
{
    double got[COV_SUMS];
    int bad = 0;

    for (size_t k = 0; k < COV_SUMS; k++)
        got[k] = COV_GUARD;
    kernel(p->x, p->y, p->stride, size.h, size.w, mean_x, mean_y, count, got);
    for (size_t k = 0; k < count; k++) {
        const double ref =
            compute_cov_kernel_scalar(p->x, p->y + k, p->stride, size.h, size.w, mean_x, mean_y[k]);
        if (cov_bits(ref) != cov_bits(got[k])) {
            (void)fprintf(stderr, "%zux%zu, block %zu of %zu: expected %a, got %a\n", size.w,
                          size.h, k, count, ref, got[k]);
            bad++;
        }
    }
    for (size_t k = count; k < COV_SUMS; k++)
        bad += cov_bits(got[k]) != cov_bits(COV_GUARD);
    return bad;
}

static int cov_plane_mismatches(speed_cov_row_fn kernel, const CovPlanes *p, CovSize size,
                                unsigned own_means)
{
    double mean_x = 127.5;
    double mean_y[SPEED_COV_ROW_MAX] = {130.25, 131.5, 96.0, 0.0, 255.0};
    int bad = 0;

    if (own_means) {
        mean_x = cov_block_mean(p->x, p->stride, size);
        for (size_t k = 0; k < SPEED_COV_ROW_MAX; k++)
            mean_y[k] = cov_block_mean(p->y + k, p->stride, size);
    }
    for (size_t count = 1; count <= SPEED_COV_ROW_MAX; count++)
        bad += cov_row_mismatches(kernel, p, size, mean_x, mean_y, count);
    return bad;
}

static char *check_cov_size(speed_cov_row_fn kernel, CovSize size, unsigned layout)
{
    CovPlanes p = {0};
    int bad = 0;

    if (!cov_planes_alloc(&p, size, layout)) {
        cov_planes_free(&p);
        return "covariance plane allocation failed";
    }
    const size_t n = (size.h - 1) * p.stride + size.w + SPEED_COV_ROW_MAX - 1;
    for (unsigned pattern = 0; pattern < COV_PATTERNS; pattern++) {
        const uint32_t seed = 0xc0ffee00u + pattern * 977u + (uint32_t)size.w;
        cov_fill(p.x, n, pattern, seed);
        cov_fill(p.y, n, pattern, seed ^ 0xA5A5A5A5u);
        bad += cov_plane_mismatches(kernel, &p, size, 0);
        bad += cov_plane_mismatches(kernel, &p, size, 1);
    }
    cov_planes_free(&p);
    mu_assert("a covariance row kernel does not return compute_cov_kernel_scalar's bits", bad == 0);
    return NULL;
}

static char *check_cov_row(speed_cov_row_fn kernel)
{
    for (size_t s = 0; s < sizeof(cov_sizes) / sizeof(cov_sizes[0]); s++) {
        for (unsigned layout = 0; layout < COV_LAYOUTS; layout++)
            mu_assert_msg(check_cov_size(kernel, cov_sizes[s], layout));
    }
    return NULL;
}

/* The portable row kernel is `count` reference calls: the harness's own
 * positive control, on every architecture. */
static char *test_cov_row_scalar(void)
{
    return check_cov_row(speed_cov_row_scalar);
}

#if ARCH_AARCH64
static char *test_cov_row_neon(void)
{
    return check_cov_row(speed_cov_row_neon);
}
#endif

#if ARCH_X86

static char *test_cov_row_avx2(void)
{
    return check_cov_row(speed_cov_row_avx2);
}

#if HAVE_AVX512
static char *test_cov_row_avx512(void)
{
    return check_cov_row(speed_cov_row_avx512);
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

static const MuTest portable_tests[] = {
    MU_TEST(test_cov_row_scalar),
};

#if ARCH_AARCH64
/* NEON is baseline on aarch64: no runtime gate. */
static const MuTest aarch64_tests[] = {
    MU_TEST(test_cov_row_neon),
};
#endif

/* Run speed SIMD tests and return the first failure message, or NULL on success. */
char *run_tests(void)
{
    char *msg = mu_run_table(portable_tests, MU_TABLE_LEN(portable_tests));
#if ARCH_AARCH64
    if (!msg)
        msg = mu_run_table(aarch64_tests, MU_TABLE_LEN(aarch64_tests));
#endif
#if ARCH_X86
    /* Guarded by `if (have)` rather than an early `return NULL`: the AVX2 and
     * AVX-512 gates then read the same way, and the TU keeps exactly one
     * success exit (ADR-1138 keeps the null pointer constant spelled `NULL`
     * here for the MSVC C lane, so each extra one is measured debt). */
    if (!msg && simd_test_have_avx2()) {
        static const MuTest avx2_tests[] = {
            MU_TEST(test_cov_row_avx2),       MU_TEST(test_matmul_avx2_speed_native),
            MU_TEST(test_matmul_avx2_rect),   MU_TEST(test_matmul_avx2_tails),
            MU_TEST(test_matmul_avx2_narrow),
        };
        msg = mu_run_table(avx2_tests, MU_TABLE_LEN(avx2_tests));

#if HAVE_AVX512
        if (!msg && simd_test_have_avx512()) {
            static const MuTest avx512_tests[] = {
                MU_TEST(test_cov_row_avx512),       MU_TEST(test_matmul_avx512_speed_native),
                MU_TEST(test_matmul_avx512_rect),   MU_TEST(test_matmul_avx512_tails),
                MU_TEST(test_matmul_avx512_narrow),
            };
            msg = mu_run_table(avx512_tests, MU_TABLE_LEN(avx512_tests));
        }
#endif /* HAVE_AVX512 */
    }
#endif
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
