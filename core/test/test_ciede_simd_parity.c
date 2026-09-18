/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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
 * Bit-exact parity test: scalar vs. ciede_preprocess_{8,16}_avx2.
 *
 * The AVX2 preprocessing kernels (ciede_avx2.c) convert packed uint8/uint16
 * YUV plane rows into float buffers that feed the per-pixel ΔE2000 scalar
 * loop in ciede.c.  The conversion is a pure integer-to-float widening with
 * no rounding ambiguity, so scalar and SIMD outputs must be bit-exact
 * (identical IEEE-754 float pattern for every element).
 *
 * Two test cases:
 *
 *   test_ciede_preprocess_8_avx2_parity:
 *     Fill three uint8 planes (Y, U, V) with reproducible pseudo-random
 *     data covering [0, 255].  Run the scalar loop (verbatim from the
 *     ciede.c fallback) and ciede_preprocess_8_avx2 on the same input.
 *     Compare all three output float arrays via memcmp.
 *
 *   test_ciede_preprocess_16_avx2_parity:
 *     Same structure with uint16 planes (values in [0, 65535], covering
 *     all 16-bit magnitudes that appear in HBD content).
 *
 * Both tests exercise the SIMD inner loop (width > 8) plus the scalar tail
 * (width not a multiple of 8) in a single pass.  The width is chosen to be
 * non-multiple-of-8 so the tail path is always exercised.
 *
 * Boilerplate provided by `simd_bitexact_test.h` (ADR-0245).
 */

#include <stddef.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "test.h"
/* clang-format off — test.h has no header guard; must precede harness. */
#include "simd_bitexact_test.h"
/* clang-format on */

#if ARCH_X86
#include "feature/x86/ciede_avx2.h"
#endif

/*
 * Width is deliberately not a multiple of 8 so the AVX2 scalar-tail branch
 * is always exercised (inner loop processes floor(w/8)*8 pixels; tail covers
 * the remaining w % 8).
 */
#define CIEDE_TEST_W 43

#if ARCH_X86

/* -------------------------------------------------------------------------
 * Scalar reference implementations.
 *
 * These are verbatim copies of the fallback loops in ciede.c, reproduced
 * here so the test does not depend on internal linkage of those functions.
 * The intent is to be a faithful oracle, not DRY code sharing.
 * ---------------------------------------------------------------------- */

static void ciede_preprocess_8_scalar(const uint8_t *y_buf, const uint8_t *u_buf,
                                      const uint8_t *v_buf, float *out_y, float *out_u,
                                      float *out_v, int w)
{
    for (int j = 0; j < w; j++) {
        out_y[j] = (float)y_buf[j];
        out_u[j] = (float)u_buf[j];
        out_v[j] = (float)v_buf[j];
    }
}

static void ciede_preprocess_16_scalar(const uint16_t *y_buf, const uint16_t *u_buf,
                                       const uint16_t *v_buf, float *out_y, float *out_u,
                                       float *out_v, int w)
{
    for (int j = 0; j < w; j++) {
        out_y[j] = (float)y_buf[j];
        out_u[j] = (float)u_buf[j];
        out_v[j] = (float)v_buf[j];
    }
}

/* -------------------------------------------------------------------------
 * Test 1: 8-bit plane preprocessing parity.
 * ---------------------------------------------------------------------- */

/* Fixture for the 8-bit parity test: three uint8 input planes plus scalar-
 * and SIMD-output float planes for Y/U/V. */
typedef struct CiedeParity8 {
    uint8_t *y_in, *u_in, *v_in;
    float *y_scalar, *u_scalar, *v_scalar;
    float *y_simd, *u_simd, *v_simd;
} CiedeParity8;

static int ciede_parity8_alloc(CiedeParity8 *b, int w)
{
    const size_t plane_bytes = (size_t)w * sizeof(uint8_t);
    const size_t out_bytes = (size_t)w * sizeof(float);
    b->y_in = (uint8_t *)simd_test_aligned_malloc(plane_bytes, 32);
    b->u_in = (uint8_t *)simd_test_aligned_malloc(plane_bytes, 32);
    b->v_in = (uint8_t *)simd_test_aligned_malloc(plane_bytes, 32);
    b->y_scalar = (float *)simd_test_aligned_malloc(out_bytes, 32);
    b->u_scalar = (float *)simd_test_aligned_malloc(out_bytes, 32);
    b->v_scalar = (float *)simd_test_aligned_malloc(out_bytes, 32);
    b->y_simd = (float *)simd_test_aligned_malloc(out_bytes, 32);
    b->u_simd = (float *)simd_test_aligned_malloc(out_bytes, 32);
    b->v_simd = (float *)simd_test_aligned_malloc(out_bytes, 32);
    if (!b->y_in || !b->u_in || !b->v_in || !b->y_scalar || !b->u_scalar || !b->v_scalar ||
        !b->y_simd || !b->u_simd || !b->v_simd)
        return -1;
    return 0;
}

static void ciede_parity8_free_inputs(CiedeParity8 *b)
{
    simd_test_aligned_free(b->y_in);
    simd_test_aligned_free(b->u_in);
    simd_test_aligned_free(b->v_in);
}

static void ciede_parity8_free_outputs(CiedeParity8 *b)
{
    simd_test_aligned_free(b->y_scalar);
    simd_test_aligned_free(b->u_scalar);
    simd_test_aligned_free(b->v_scalar);
    simd_test_aligned_free(b->y_simd);
    simd_test_aligned_free(b->u_simd);
    simd_test_aligned_free(b->v_simd);
}

static void ciede_parity8_free_all(CiedeParity8 *b)
{
    ciede_parity8_free_inputs(b);
    ciede_parity8_free_outputs(b);
}

static char *test_ciede_preprocess_8_avx2_parity(void)
{
    const int w = CIEDE_TEST_W;
    const size_t out_bytes = (size_t)w * sizeof(float);

    CiedeParity8 b = {0};
    if (ciede_parity8_alloc(&b, w)) {
        ciede_parity8_free_all(&b);
        return "aligned_malloc failed";
    }

    /* Fill inputs with reproducible pseudo-random uint8 values. */
    uint32_t state = 0xc1ede2u;
    for (int j = 0; j < w; j++) {
        uint32_t r = simd_test_xorshift32(&state);
        b.y_in[j] = (uint8_t)(r & 0xFFu);
        b.u_in[j] = (uint8_t)((r >> 8) & 0xFFu);
        b.v_in[j] = (uint8_t)((r >> 16) & 0xFFu);
    }

    /* Ensure output buffers start clean so any unwritten element divergence
     * is detectable. */
    (void)memset(b.y_scalar, 0xAB, out_bytes);
    (void)memset(b.u_scalar, 0xAB, out_bytes);
    (void)memset(b.v_scalar, 0xAB, out_bytes);
    (void)memset(b.y_simd, 0xCD, out_bytes);
    (void)memset(b.u_simd, 0xCD, out_bytes);
    (void)memset(b.v_simd, 0xCD, out_bytes);

    ciede_preprocess_8_scalar(b.y_in, b.u_in, b.v_in, b.y_scalar, b.u_scalar, b.v_scalar, w);
    ciede_preprocess_8_avx2(b.y_in, b.u_in, b.v_in, b.y_simd, b.u_simd, b.v_simd, w);

    ciede_parity8_free_inputs(&b);

    SIMD_BITEXACT_ASSERT_MEMCMP(b.y_scalar, b.y_simd, out_bytes, "ciede_preprocess_8_avx2 Y plane");
    SIMD_BITEXACT_ASSERT_MEMCMP(b.u_scalar, b.u_simd, out_bytes, "ciede_preprocess_8_avx2 U plane");
    SIMD_BITEXACT_ASSERT_MEMCMP(b.v_scalar, b.v_simd, out_bytes, "ciede_preprocess_8_avx2 V plane");

    ciede_parity8_free_outputs(&b);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Test 2: 16-bit plane preprocessing parity.
 * ---------------------------------------------------------------------- */

/* Fixture for the 16-bit parity test: three uint16 input planes plus
 * scalar- and SIMD-output float planes for Y/U/V. */
typedef struct CiedeParity16 {
    uint16_t *y_in, *u_in, *v_in;
    float *y_scalar, *u_scalar, *v_scalar;
    float *y_simd, *u_simd, *v_simd;
} CiedeParity16;

static int ciede_parity16_alloc(CiedeParity16 *b, int w)
{
    const size_t plane_bytes = (size_t)w * sizeof(uint16_t);
    const size_t out_bytes = (size_t)w * sizeof(float);
    b->y_in = (uint16_t *)simd_test_aligned_malloc(plane_bytes, 32);
    b->u_in = (uint16_t *)simd_test_aligned_malloc(plane_bytes, 32);
    b->v_in = (uint16_t *)simd_test_aligned_malloc(plane_bytes, 32);
    b->y_scalar = (float *)simd_test_aligned_malloc(out_bytes, 32);
    b->u_scalar = (float *)simd_test_aligned_malloc(out_bytes, 32);
    b->v_scalar = (float *)simd_test_aligned_malloc(out_bytes, 32);
    b->y_simd = (float *)simd_test_aligned_malloc(out_bytes, 32);
    b->u_simd = (float *)simd_test_aligned_malloc(out_bytes, 32);
    b->v_simd = (float *)simd_test_aligned_malloc(out_bytes, 32);
    if (!b->y_in || !b->u_in || !b->v_in || !b->y_scalar || !b->u_scalar || !b->v_scalar ||
        !b->y_simd || !b->u_simd || !b->v_simd)
        return -1;
    return 0;
}

static void ciede_parity16_free_inputs(CiedeParity16 *b)
{
    simd_test_aligned_free(b->y_in);
    simd_test_aligned_free(b->u_in);
    simd_test_aligned_free(b->v_in);
}

static void ciede_parity16_free_outputs(CiedeParity16 *b)
{
    simd_test_aligned_free(b->y_scalar);
    simd_test_aligned_free(b->u_scalar);
    simd_test_aligned_free(b->v_scalar);
    simd_test_aligned_free(b->y_simd);
    simd_test_aligned_free(b->u_simd);
    simd_test_aligned_free(b->v_simd);
}

static void ciede_parity16_free_all(CiedeParity16 *b)
{
    ciede_parity16_free_inputs(b);
    ciede_parity16_free_outputs(b);
}

static char *test_ciede_preprocess_16_avx2_parity(void)
{
    const int w = CIEDE_TEST_W;
    const size_t out_bytes = (size_t)w * sizeof(float);

    CiedeParity16 b = {0};
    if (ciede_parity16_alloc(&b, w)) {
        ciede_parity16_free_all(&b);
        return "aligned_malloc failed";
    }

    /*
     * Fill with full-range uint16 values so the test exercises values above
     * the 10-bit range (> 1023) and verifies the AVX2 zero-extension path
     * handles them correctly.
     */
    simd_test_fill_random_u16(b.y_in, (size_t)w, 0xFFFF, 0xde16u);
    simd_test_fill_random_u16(b.u_in, (size_t)w, 0xFFFF, 0xde16u + 1u);
    simd_test_fill_random_u16(b.v_in, (size_t)w, 0xFFFF, 0xde16u + 2u);

    (void)memset(b.y_scalar, 0xAB, out_bytes);
    (void)memset(b.u_scalar, 0xAB, out_bytes);
    (void)memset(b.v_scalar, 0xAB, out_bytes);
    (void)memset(b.y_simd, 0xCD, out_bytes);
    (void)memset(b.u_simd, 0xCD, out_bytes);
    (void)memset(b.v_simd, 0xCD, out_bytes);

    ciede_preprocess_16_scalar(b.y_in, b.u_in, b.v_in, b.y_scalar, b.u_scalar, b.v_scalar, w);
    ciede_preprocess_16_avx2(b.y_in, b.u_in, b.v_in, b.y_simd, b.u_simd, b.v_simd, w);

    ciede_parity16_free_inputs(&b);

    SIMD_BITEXACT_ASSERT_MEMCMP(b.y_scalar, b.y_simd, out_bytes,
                                "ciede_preprocess_16_avx2 Y plane");
    SIMD_BITEXACT_ASSERT_MEMCMP(b.u_scalar, b.u_simd, out_bytes,
                                "ciede_preprocess_16_avx2 U plane");
    SIMD_BITEXACT_ASSERT_MEMCMP(b.v_scalar, b.v_simd, out_bytes,
                                "ciede_preprocess_16_avx2 V plane");

    ciede_parity16_free_outputs(&b);
    return NULL;
}

#endif /* ARCH_X86 */

char *run_tests(void)
{
#if ARCH_X86
    if (!simd_test_have_avx2()) {
        return NULL;
    }
    mu_run_test(test_ciede_preprocess_8_avx2_parity);
    mu_run_test(test_ciede_preprocess_16_avx2_parity);
#else
    (void)fprintf(stderr, "skipping SIMD parity: non-x86 arch\n");
#endif
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
