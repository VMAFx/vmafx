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

static char *test_ciede_preprocess_8_avx2_parity(void)
{
    const int w = CIEDE_TEST_W;
    const size_t plane_bytes = (size_t)w * sizeof(uint8_t);
    const size_t out_bytes = (size_t)w * sizeof(float);

    uint8_t *y_in = (uint8_t *)simd_test_aligned_malloc(plane_bytes, 32);
    uint8_t *u_in = (uint8_t *)simd_test_aligned_malloc(plane_bytes, 32);
    uint8_t *v_in = (uint8_t *)simd_test_aligned_malloc(plane_bytes, 32);

    float *y_scalar = (float *)simd_test_aligned_malloc(out_bytes, 32);
    float *u_scalar = (float *)simd_test_aligned_malloc(out_bytes, 32);
    float *v_scalar = (float *)simd_test_aligned_malloc(out_bytes, 32);

    float *y_simd = (float *)simd_test_aligned_malloc(out_bytes, 32);
    float *u_simd = (float *)simd_test_aligned_malloc(out_bytes, 32);
    float *v_simd = (float *)simd_test_aligned_malloc(out_bytes, 32);

    if (!y_in || !u_in || !v_in || !y_scalar || !u_scalar || !v_scalar || !y_simd || !u_simd ||
        !v_simd) {
        simd_test_aligned_free(y_in);
        simd_test_aligned_free(u_in);
        simd_test_aligned_free(v_in);
        simd_test_aligned_free(y_scalar);
        simd_test_aligned_free(u_scalar);
        simd_test_aligned_free(v_scalar);
        simd_test_aligned_free(y_simd);
        simd_test_aligned_free(u_simd);
        simd_test_aligned_free(v_simd);
        return "aligned_malloc failed";
    }

    /* Fill inputs with reproducible pseudo-random uint8 values. */
    uint32_t state = 0xc1ede2u;
    for (int j = 0; j < w; j++) {
        uint32_t r = simd_test_xorshift32(&state);
        y_in[j] = (uint8_t)(r & 0xFFu);
        u_in[j] = (uint8_t)((r >> 8) & 0xFFu);
        v_in[j] = (uint8_t)((r >> 16) & 0xFFu);
    }

    /* Ensure output buffers start clean so any unwritten element divergence
     * is detectable. */
    (void)memset(y_scalar, 0xAB, out_bytes);
    (void)memset(u_scalar, 0xAB, out_bytes);
    (void)memset(v_scalar, 0xAB, out_bytes);
    (void)memset(y_simd, 0xCD, out_bytes);
    (void)memset(u_simd, 0xCD, out_bytes);
    (void)memset(v_simd, 0xCD, out_bytes);

    ciede_preprocess_8_scalar(y_in, u_in, v_in, y_scalar, u_scalar, v_scalar, w);
    ciede_preprocess_8_avx2(y_in, u_in, v_in, y_simd, u_simd, v_simd, w);

    simd_test_aligned_free(y_in);
    simd_test_aligned_free(u_in);
    simd_test_aligned_free(v_in);

    SIMD_BITEXACT_ASSERT_MEMCMP(y_scalar, y_simd, out_bytes, "ciede_preprocess_8_avx2 Y plane");
    SIMD_BITEXACT_ASSERT_MEMCMP(u_scalar, u_simd, out_bytes, "ciede_preprocess_8_avx2 U plane");
    SIMD_BITEXACT_ASSERT_MEMCMP(v_scalar, v_simd, out_bytes, "ciede_preprocess_8_avx2 V plane");

    simd_test_aligned_free(y_scalar);
    simd_test_aligned_free(u_scalar);
    simd_test_aligned_free(v_scalar);
    simd_test_aligned_free(y_simd);
    simd_test_aligned_free(u_simd);
    simd_test_aligned_free(v_simd);

    return NULL;
}

/* -------------------------------------------------------------------------
 * Test 2: 16-bit plane preprocessing parity.
 * ---------------------------------------------------------------------- */

static char *test_ciede_preprocess_16_avx2_parity(void)
{
    const int w = CIEDE_TEST_W;
    const size_t plane_bytes = (size_t)w * sizeof(uint16_t);
    const size_t out_bytes = (size_t)w * sizeof(float);

    uint16_t *y_in = (uint16_t *)simd_test_aligned_malloc(plane_bytes, 32);
    uint16_t *u_in = (uint16_t *)simd_test_aligned_malloc(plane_bytes, 32);
    uint16_t *v_in = (uint16_t *)simd_test_aligned_malloc(plane_bytes, 32);

    float *y_scalar = (float *)simd_test_aligned_malloc(out_bytes, 32);
    float *u_scalar = (float *)simd_test_aligned_malloc(out_bytes, 32);
    float *v_scalar = (float *)simd_test_aligned_malloc(out_bytes, 32);

    float *y_simd = (float *)simd_test_aligned_malloc(out_bytes, 32);
    float *u_simd = (float *)simd_test_aligned_malloc(out_bytes, 32);
    float *v_simd = (float *)simd_test_aligned_malloc(out_bytes, 32);

    if (!y_in || !u_in || !v_in || !y_scalar || !u_scalar || !v_scalar || !y_simd || !u_simd ||
        !v_simd) {
        simd_test_aligned_free(y_in);
        simd_test_aligned_free(u_in);
        simd_test_aligned_free(v_in);
        simd_test_aligned_free(y_scalar);
        simd_test_aligned_free(u_scalar);
        simd_test_aligned_free(v_scalar);
        simd_test_aligned_free(y_simd);
        simd_test_aligned_free(u_simd);
        simd_test_aligned_free(v_simd);
        return "aligned_malloc failed";
    }

    /*
     * Fill with full-range uint16 values so the test exercises values above
     * the 10-bit range (> 1023) and verifies the AVX2 zero-extension path
     * handles them correctly.
     */
    simd_test_fill_random_u16(y_in, (size_t)w, 0xFFFF, 0xde16u);
    simd_test_fill_random_u16(u_in, (size_t)w, 0xFFFF, 0xde16u + 1u);
    simd_test_fill_random_u16(v_in, (size_t)w, 0xFFFF, 0xde16u + 2u);

    (void)memset(y_scalar, 0xAB, out_bytes);
    (void)memset(u_scalar, 0xAB, out_bytes);
    (void)memset(v_scalar, 0xAB, out_bytes);
    (void)memset(y_simd, 0xCD, out_bytes);
    (void)memset(u_simd, 0xCD, out_bytes);
    (void)memset(v_simd, 0xCD, out_bytes);

    ciede_preprocess_16_scalar(y_in, u_in, v_in, y_scalar, u_scalar, v_scalar, w);
    ciede_preprocess_16_avx2(y_in, u_in, v_in, y_simd, u_simd, v_simd, w);

    simd_test_aligned_free(y_in);
    simd_test_aligned_free(u_in);
    simd_test_aligned_free(v_in);

    SIMD_BITEXACT_ASSERT_MEMCMP(y_scalar, y_simd, out_bytes, "ciede_preprocess_16_avx2 Y plane");
    SIMD_BITEXACT_ASSERT_MEMCMP(u_scalar, u_simd, out_bytes, "ciede_preprocess_16_avx2 U plane");
    SIMD_BITEXACT_ASSERT_MEMCMP(v_scalar, v_simd, out_bytes, "ciede_preprocess_16_avx2 V plane");

    simd_test_aligned_free(y_scalar);
    simd_test_aligned_free(u_scalar);
    simd_test_aligned_free(v_scalar);
    simd_test_aligned_free(y_simd);
    simd_test_aligned_free(u_simd);
    simd_test_aligned_free(v_simd);

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
