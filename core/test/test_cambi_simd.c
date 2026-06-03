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
 * Bit-exact parity tests for calculate_c_values_row_avx512 and
 * calculate_c_values_row_neon against a local scalar reference (ADR-0452).
 *
 * CAMBI is a pure integer pipeline: histogram counts are uint16, and the
 * per-column c-value is independent of other columns.  SIMD and scalar
 * outputs are byte-identical; SIMD_BITEXACT_ASSERT_MEMCMP is the correct
 * assertion — no ULP tolerance (ADR-0138/0139 contract).
 *
 * The scalar reference is implemented below as calculate_c_values_row_scalar
 * (verbatim from cambi.c::calculate_c_values_row, adapting for the fact that
 * the production version uses an in-TU static LUT pointer).  We construct
 * the LUT locally and pass it explicitly, matching the SIMD kernel's API.
 *
 * Test dimensions: 37-wide (not a multiple of 8 or 16 — exercises tails),
 * 1 row.  The mask pattern (every 3rd column inactive) exercises both the
 * active-pixel inner loop and the masked-out skip path.
 *
 * Boilerplate (xorshift PRNG, aligned alloc, AVX2/AVX-512 gate, memcmp
 * assertion macros) provided by simd_bitexact_test.h (ADR-0245).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "test.h"
/* clang-format off — test.h has no header guard; must precede harness. */
#include "simd_bitexact_test.h"
/* clang-format on */

#if ARCH_X86
#include "x86/cpu.h"
#if HAVE_AVX512
#include "feature/x86/cambi_avx512.h"
#endif
#endif
#if ARCH_AARCH64
#include "feature/arm64/cambi_neon.h"
#endif

/* ---- Local scalar reference ---------------------------------------- */

/*
 * Inline scalar implementation of calculate_c_values_row.
 *
 * Mirrors the production function in libvmaf/src/feature/cambi.c verbatim.
 * The only difference: the production version reads the reciprocal LUT
 * from a file-scope global; here we accept it as an explicit parameter
 * (which the SIMD kernels also do, so the calling convention is unified).
 *
 * This function is the ground truth for the parity assertions below.
 */
static void calculate_c_values_row_scalar(float *c_values, const uint16_t *histograms,
                                          const uint16_t *image, const uint16_t *mask, int row,
                                          int width, ptrdiff_t stride, const uint16_t num_diffs,
                                          const uint16_t *tvi_thresholds, uint16_t vlt_luma,
                                          const int *diff_weights, const int *all_diffs,
                                          const float *reciprocal_lut)
{
    int v_lo_signed_sc = (int)vlt_luma - 3 * (int)num_diffs + 1;
    uint16_t v_band_base = v_lo_signed_sc > 0 ? (uint16_t)v_lo_signed_sc : 0;
    uint16_t v_band_size = tvi_thresholds[num_diffs - 1] + 1 - v_band_base;

    const uint16_t *image_row = &image[row * stride];
    const uint16_t *mask_row = &mask[row * stride];
    float *c_row = &c_values[row * width];

    for (int col = 0; col < width; col++) {
        if (!mask_row[col]) {
            c_row[col] = 0.0f;
            continue;
        }
        uint16_t value = (uint16_t)(image_row[col] + num_diffs);
        int compact_v_signed = (int)image_row[col] - (int)v_band_base;
        if ((unsigned)compact_v_signed >= v_band_size) {
            c_row[col] = 0.0f;
            continue;
        }
        uint16_t compact_v_sc = (uint16_t)compact_v_signed;
        uint16_t p_0 = histograms[compact_v_sc * width + col];
        float c_v = 0.0f;
        for (int d = 0; d < num_diffs; d++) {
            if ((value <= tvi_thresholds[d]) &&
                ((value + all_diffs[num_diffs + d + 1]) > vlt_luma)) {
                int idx1 = compact_v_signed + all_diffs[num_diffs + d + 1];
                int idx2 = compact_v_signed + all_diffs[num_diffs - d - 1];
                uint16_t p_1 = histograms[idx1 * width + col];
                uint16_t p_2 = (idx2 >= 0) ? histograms[idx2 * width + col] : 0;
                uint16_t p_max = (p_1 > p_2) ? p_1 : p_2;
                float val = (float)(diff_weights[d] * p_0 * p_max) * reciprocal_lut[p_max + p_0];
                if (val > c_v)
                    c_v = val;
            }
        }
        c_row[col] = c_v;
    }
}

/* ---- Fixture parameters -------------------------------------------- */

/* Width not a multiple of 8 or 16 — exercises scalar tail on every ISA. */
#define TEST_WIDTH 37
/* Stride wider than width to verify offset arithmetic. */
#define TEST_STRIDE 40
#define TEST_ROW 0
#define NUM_DIFFS 5

/* reciprocal_lut[i] = 1/i (i>0), 0 (i=0).
 * Max denom = 2 * max_histogram_count.  Fixture counts bounded to 0..15,
 * so denom <= 30; 256 is safely larger. */
#define RECIP_LUT_SIZE 256

/* Histogram band size: generous so compact indices stay in range. */
#define BAND_SIZE 256

static float g_reciprocal_lut[RECIP_LUT_SIZE];

static void init_recip_lut(void)
{
    g_reciprocal_lut[0] = 0.0f;
    for (int i = 1; i < RECIP_LUT_SIZE; i++) {
        g_reciprocal_lut[i] = 1.0f / (float)i;
    }
}

/* ---- Fixture type --------------------------------------------------- */

typedef struct {
    uint16_t image[TEST_STRIDE];
    uint16_t mask[TEST_STRIDE];
    uint16_t histograms[BAND_SIZE * TEST_WIDTH];
    uint16_t tvi_thresholds[NUM_DIFFS];
    int diff_weights[NUM_DIFFS];
    /* 2*NUM_DIFFS+1 entries: symmetric around 0. */
    int all_diffs[2 * NUM_DIFFS + 1];
    uint16_t vlt_luma;
} CambiRowFixture;

static void build_fixture(CambiRowFixture *fx, uint32_t seed)
{
    uint32_t state = seed;

    /* 10-bit luma values — stay within RECIP_LUT_SIZE denom budget. */
    for (int j = 0; j < TEST_STRIDE; j++) {
        fx->image[j] = (uint16_t)(simd_test_xorshift32(&state) & 0x3FFu);
    }

    /* Mask: every 3rd column inactive — exercises both code paths. */
    for (int j = 0; j < TEST_STRIDE; j++) {
        fx->mask[j] = (j % 3 != 0) ? 1u : 0u;
    }

    /* Histogram counts in [0, 7]: p0*p_max <= 49; max denom = 14 < RECIP_LUT_SIZE. */
    for (int i = 0; i < BAND_SIZE * TEST_WIDTH; i++) {
        fx->histograms[i] = (uint16_t)(simd_test_xorshift32(&state) & 0x7u);
    }

    /* TVI thresholds: increasing, step 30 starting at 100.
     * With vlt_luma=50, num_diffs=5: v_band_base=36, v_band_size=185.
     * Max histogram index = 184*TEST_WIDTH+36 = 6844, well within
     * BAND_SIZE*TEST_WIDTH=9472 (the histogram buffer size). */
    fx->tvi_thresholds[0] = 100;
    for (int d = 1; d < NUM_DIFFS; d++) {
        fx->tvi_thresholds[d] = (uint16_t)(fx->tvi_thresholds[d - 1] + 30);
    }

    /* Diff weights: small positive values. */
    for (int d = 0; d < NUM_DIFFS; d++) {
        fx->diff_weights[d] = d + 1;
    }

    /* all_diffs: symmetric, length 2*NUM_DIFFS+1. */
    fx->all_diffs[NUM_DIFFS] = 0;
    for (int d = 0; d < NUM_DIFFS; d++) {
        fx->all_diffs[NUM_DIFFS + d + 1] = d + 1;
        fx->all_diffs[NUM_DIFFS - d - 1] = -(d + 1);
    }

    /* vlt_luma: low enough that most pixels satisfy (value + delta) > vlt_luma. */
    fx->vlt_luma = 50;
}

/* ---- Helpers to call both scalar and SIMD with the same args ------- */

#define CALL_SCALAR(out, fx)                                                                       \
    calculate_c_values_row_scalar((out), (fx).histograms, (fx).image, (fx).mask, TEST_ROW,         \
                                  TEST_WIDTH, TEST_STRIDE, NUM_DIFFS, (fx).tvi_thresholds,         \
                                  (fx).vlt_luma, (fx).diff_weights, (fx).all_diffs,                \
                                  g_reciprocal_lut)

/* ---- AVX-512 parity tests ------------------------------------------ */

#if ARCH_X86

#if HAVE_AVX512
static char *test_avx512_parity_seed_a(void)
{
    CambiRowFixture fx;
    build_fixture(&fx, 0xABCD1234u);

    float scalar_out[TEST_WIDTH];
    float simd_out[TEST_WIDTH];
    memset(scalar_out, 0, sizeof(scalar_out));
    memset(simd_out, 0, sizeof(simd_out));

    CALL_SCALAR(scalar_out, fx);
    calculate_c_values_row_avx512(simd_out, fx.histograms, fx.image, fx.mask, TEST_ROW, TEST_WIDTH,
                                  TEST_STRIDE, NUM_DIFFS, fx.tvi_thresholds, fx.vlt_luma,
                                  fx.diff_weights, fx.all_diffs, g_reciprocal_lut);

    SIMD_BITEXACT_ASSERT_MEMCMP(scalar_out, simd_out, sizeof(scalar_out), "avx512 parity seed_a");
    return NULL;
}
#endif

#if HAVE_AVX512
static char *test_avx512_parity_seed_b(void)
{
    CambiRowFixture fx;
    build_fixture(&fx, 0x12345678u);

    float scalar_out[TEST_WIDTH];
    float simd_out[TEST_WIDTH];
    memset(scalar_out, 0, sizeof(scalar_out));
    memset(simd_out, 0, sizeof(simd_out));

    CALL_SCALAR(scalar_out, fx);
    calculate_c_values_row_avx512(simd_out, fx.histograms, fx.image, fx.mask, TEST_ROW, TEST_WIDTH,
                                  TEST_STRIDE, NUM_DIFFS, fx.tvi_thresholds, fx.vlt_luma,
                                  fx.diff_weights, fx.all_diffs, g_reciprocal_lut);

    SIMD_BITEXACT_ASSERT_MEMCMP(scalar_out, simd_out, sizeof(scalar_out), "avx512 parity seed_b");
    return NULL;
}
#endif

#if HAVE_AVX512
static char *test_avx512_all_masked_out(void)
{
    CambiRowFixture fx;
    build_fixture(&fx, 0xDEADBEEFu);
    memset(fx.mask, 0, sizeof(fx.mask));

    float scalar_out[TEST_WIDTH];
    float simd_out[TEST_WIDTH];
    /* Pre-fill with sentinel to confirm zeros are written by both paths. */
    for (int i = 0; i < TEST_WIDTH; i++) {
        scalar_out[i] = 99.0f;
        simd_out[i] = 99.0f;
    }

    CALL_SCALAR(scalar_out, fx);
    calculate_c_values_row_avx512(simd_out, fx.histograms, fx.image, fx.mask, TEST_ROW, TEST_WIDTH,
                                  TEST_STRIDE, NUM_DIFFS, fx.tvi_thresholds, fx.vlt_luma,
                                  fx.diff_weights, fx.all_diffs, g_reciprocal_lut);

    SIMD_BITEXACT_ASSERT_MEMCMP(scalar_out, simd_out, sizeof(scalar_out), "avx512 all-masked-out");
    return NULL;
}
#endif

#endif /* ARCH_X86 */

/* ---- NEON parity tests --------------------------------------------- */

#if ARCH_AARCH64

static char *test_neon_parity_seed_a(void)
{
    CambiRowFixture fx;
    build_fixture(&fx, 0xABCD1234u);

    float scalar_out[TEST_WIDTH];
    float simd_out[TEST_WIDTH];
    memset(scalar_out, 0, sizeof(scalar_out));
    memset(simd_out, 0, sizeof(simd_out));

    CALL_SCALAR(scalar_out, fx);
    calculate_c_values_row_neon(simd_out, fx.histograms, fx.image, fx.mask, TEST_ROW, TEST_WIDTH,
                                TEST_STRIDE, NUM_DIFFS, fx.tvi_thresholds, fx.vlt_luma,
                                fx.diff_weights, fx.all_diffs, g_reciprocal_lut);

    SIMD_BITEXACT_ASSERT_MEMCMP(scalar_out, simd_out, sizeof(scalar_out), "neon parity seed_a");
    return NULL;
}

static char *test_neon_parity_seed_b(void)
{
    CambiRowFixture fx;
    build_fixture(&fx, 0x12345678u);

    float scalar_out[TEST_WIDTH];
    float simd_out[TEST_WIDTH];
    memset(scalar_out, 0, sizeof(scalar_out));
    memset(simd_out, 0, sizeof(simd_out));

    CALL_SCALAR(scalar_out, fx);
    calculate_c_values_row_neon(simd_out, fx.histograms, fx.image, fx.mask, TEST_ROW, TEST_WIDTH,
                                TEST_STRIDE, NUM_DIFFS, fx.tvi_thresholds, fx.vlt_luma,
                                fx.diff_weights, fx.all_diffs, g_reciprocal_lut);

    SIMD_BITEXACT_ASSERT_MEMCMP(scalar_out, simd_out, sizeof(scalar_out), "neon parity seed_b");
    return NULL;
}

static char *test_neon_all_masked_out(void)
{
    CambiRowFixture fx;
    build_fixture(&fx, 0xDEADBEEFu);
    memset(fx.mask, 0, sizeof(fx.mask));

    float scalar_out[TEST_WIDTH];
    float simd_out[TEST_WIDTH];
    for (int i = 0; i < TEST_WIDTH; i++) {
        scalar_out[i] = 99.0f;
        simd_out[i] = 99.0f;
    }

    CALL_SCALAR(scalar_out, fx);
    calculate_c_values_row_neon(simd_out, fx.histograms, fx.image, fx.mask, TEST_ROW, TEST_WIDTH,
                                TEST_STRIDE, NUM_DIFFS, fx.tvi_thresholds, fx.vlt_luma,
                                fx.diff_weights, fx.all_diffs, g_reciprocal_lut);

    SIMD_BITEXACT_ASSERT_MEMCMP(scalar_out, simd_out, sizeof(scalar_out), "neon all-masked-out");
    return NULL;
}

#endif /* ARCH_AARCH64 */

/* ---- Test runner ---------------------------------------------------- */

char *run_tests(void)
{
    init_recip_lut();

#if ARCH_X86
    {
        const unsigned cpu_flags = vmaf_get_cpu_flags_x86();
        if (cpu_flags & VMAF_X86_CPU_FLAG_AVX512) {
#if HAVE_AVX512
            mu_run_test(test_avx512_parity_seed_a);
            mu_run_test(test_avx512_parity_seed_b);
            mu_run_test(test_avx512_all_masked_out);
#endif
        } else {
            (void)fprintf(stderr, "skipping AVX-512 tests: CPU lacks AVX-512\n");
        }
    }
#endif

#if ARCH_AARCH64
    mu_run_test(test_neon_parity_seed_a);
    mu_run_test(test_neon_parity_seed_b);
    mu_run_test(test_neon_all_masked_out);
#endif

#if !ARCH_X86 && !ARCH_AARCH64
    (void)fprintf(stderr, "skipping: arch lacks cambi SIMD\n");
#endif

    return NULL;
}
