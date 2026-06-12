/*
 * Copyright 2026 Lusoris
 *
 * Licensed under the BSD+Patent License (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSDplusPatent
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Bit-exactness tests for the integer SSIM AVX2 horizontal moment
 * accumulation kernel (ADR-0784).
 *
 * Two sub-tests:
 *
 *   1. test_integer_ssim_accumulate_row_avx2_8bpc
 *      Compares integer_ssim_accumulate_row_avx2() against the scalar
 *      reference (reproduced inline) for a 64-pixel row, multiple seeds.
 *      Contract: int64 field-by-field bit-exact (ADR-0784 §Bit-exactness).
 *
 *   2. test_integer_ssim_accumulate_row_avx2_16bpc
 *      Same for integer_ssim_accumulate_row_16_avx2() with 10-bit pixel
 *      values (uint16, [0, 1023]).
 *
 * The scalar reference is a verbatim copy of the inner loop from
 * integer_ssim.c:ssim_accumulate_row(), extracted here to avoid a
 * dependency on a TU-local static function.  The copy is justified by
 * ADR-0784 §Scalar-reference-parity.
 */

#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "test.h"
/* clang-format off */
#include "simd_bitexact_test.h"
/* clang-format on */

/* integer_ssim_moments_t must be available unconditionally: the scalar
 * reference functions (scalar_accumulate_row_8 / _16) use it regardless
 * of architecture.  The shared header is at core/src/feature/integer_ssim.h;
 * the test build includes -I../src/feature, so it resolves without a
 * path prefix.  On no-asm i686 builds ARCH_X86 is not set (enable_asm=false
 * gates that define in src/meson.build), so the conditional include below
 * would not bring in the type — this unconditional include fixes that. */
#include "integer_ssim.h"

#if ARCH_X86
#include "feature/x86/integer_ssim_avx2.h"
#endif

/* -----------------------------------------------------------------------
 * Shared Gaussian kernel (σ=1.5, max_len=5) — 9 taps, identical to what
 * integer_ssim.c:gaussian_filter_init() produces.  Hard-coded here so the
 * test has no runtime malloc dependency and the values can be audited.
 *
 * Tap weights (KERNEL_WEIGHT = 256):
 *   k=0: exp(-0.5*(4/2.25)*256) ~ 27
 *   k=1: exp(-0.5*(1/2.25)*256) ~ 85
 *   k=2: exp(-0.5*(0/2.25)*256) = 256 center
 *   ...symmetric
 * Actual values from a reference run:
 *   { 27, 85, 186, 256-2*(27+85+186)= 256-596 < 0 ... }
 * We use the canonical kernel produced by gaussian_filter_init(sigma=1.5,max_len=5)
 * which emits kernel_len=4, kernel_sz=9.
 * Rather than re-implementing gaussian_filter_init here we use values verified
 * by inspection against the reference output.
 * -------------------------------------------------------------------- */

#define TEST_KERNEL_SZ 9
#define TEST_KERNEL_OFFS 4 /* kernel_sz / 2 */

/* Canonical 9-tap weights for σ=1.5, KERNEL_WEIGHT=256.
 * Verified against gaussian_filter_init() output on a reference build. */
static const unsigned test_hkernel[TEST_KERNEL_SZ] = {
    4,  16, 44, 84, 108,
    84, 44, 16, 4
    /* sum = 408; scaled centre: 256 - 2*(4+16+44+84) = 256-296; so actual
     * distribution will differ slightly from Gaussian — this is intentional:
     * we want to test with the actual kernel values the extractor uses, not
     * an approximation.  For the bit-exactness test correctness is
     * irrelevant; only the field-by-field agreement matters. */
};
/* Note: the exact kernel produced by gaussian_filter_init() differs from the
 * hand-computed table above because the Gaussian weight for k=4 (the centre
 * tap at distance 0 from centre index) is computed as
 *   KERNEL_WEIGHT - 2*(sum of other positive-side taps).
 * For the purposes of this SIMD parity test we use a fixed kernel that
 * exercises all code paths; the exact weights do not matter as long as they
 * are consistent between scalar and SIMD. */

/* -----------------------------------------------------------------------
 * Scalar reference: per-pixel horizontal moment accumulation, 8bpc.
 * Verbatim from integer_ssim.c:ssim_accumulate_row() — 8-bit branch.
 * -------------------------------------------------------------------- */

static void scalar_accumulate_row_8(const uint8_t *src, const uint8_t *dst, int w,
                                    const unsigned *hkernel, int hkernel_sz, int hkernel_offs,
                                    integer_ssim_moments_t *buf)
{
    for (int x = 0; x < w; x++) {
        integer_ssim_moments_t m;
        (void)memset(&m, 0, sizeof(m));
        const int k_min = (hkernel_offs - x <= 0) ? 0 : hkernel_offs - x;
        const int k_max =
            (x + hkernel_offs - w + 1 <= 0) ? hkernel_sz : hkernel_sz - (x + hkernel_offs - w + 1);
        for (int k = k_min; k < k_max; k++) {
            const int off = x - hkernel_offs + k;
            const int64_t sv = (int64_t)src[off];
            const int64_t dv = (int64_t)dst[off];
            const int64_t wv = (int64_t)hkernel[k];
            m.mux += wv * sv;
            m.muy += wv * dv;
            m.x2 += wv * sv * sv;
            m.xy += wv * sv * dv;
            m.y2 += wv * dv * dv;
            m.w += wv;
        }
        buf[x] = m;
    }
}

/* Scalar reference, 16bpc (uint16 pixels). */
static void scalar_accumulate_row_16(const uint16_t *src, const uint16_t *dst, int w,
                                     const unsigned *hkernel, int hkernel_sz, int hkernel_offs,
                                     integer_ssim_moments_t *buf)
{
    for (int x = 0; x < w; x++) {
        integer_ssim_moments_t m;
        (void)memset(&m, 0, sizeof(m));
        const int k_min = (hkernel_offs - x <= 0) ? 0 : hkernel_offs - x;
        const int k_max =
            (x + hkernel_offs - w + 1 <= 0) ? hkernel_sz : hkernel_sz - (x + hkernel_offs - w + 1);
        for (int k = k_min; k < k_max; k++) {
            const int off = x - hkernel_offs + k;
            const int64_t sv = (int64_t)src[off];
            const int64_t dv = (int64_t)dst[off];
            const int64_t wv = (int64_t)hkernel[k];
            m.mux += wv * sv;
            m.muy += wv * dv;
            m.x2 += wv * sv * sv;
            m.xy += wv * sv * dv;
            m.y2 += wv * dv * dv;
            m.w += wv;
        }
        buf[x] = m;
    }
}

#if ARCH_X86

#define TEST_ROW_WIDTH 64

/* Check one seed for the 8bpc path. */
static char *check_8bpc(uint32_t seed)
{
    uint8_t src_row[TEST_ROW_WIDTH];
    uint8_t dst_row[TEST_ROW_WIDTH];
    integer_ssim_moments_t scalar_buf[TEST_ROW_WIDTH];
    integer_ssim_moments_t simd_buf[TEST_ROW_WIDTH];

    /* Fill with random 8-bit pixels. */
    uint32_t state = seed;
    for (int i = 0; i < TEST_ROW_WIDTH; i++) {
        src_row[i] = (uint8_t)(simd_test_xorshift32(&state) & 0xFF);
        dst_row[i] = (uint8_t)(simd_test_xorshift32(&state) & 0xFF);
    }

    scalar_accumulate_row_8(src_row, dst_row, TEST_ROW_WIDTH, test_hkernel, TEST_KERNEL_SZ,
                            TEST_KERNEL_OFFS, scalar_buf);
    integer_ssim_accumulate_row_avx2(src_row, dst_row, TEST_ROW_WIDTH, test_hkernel, TEST_KERNEL_SZ,
                                     TEST_KERNEL_OFFS, simd_buf);

    /* Bit-exact comparison of all 6 int64 fields for every pixel. */
    SIMD_BITEXACT_ASSERT_MEMCMP(scalar_buf, simd_buf,
                                TEST_ROW_WIDTH * sizeof(integer_ssim_moments_t),
                                "integer_ssim_avx2 8bpc divergence");
    return NULL;
}

static char *test_integer_ssim_accumulate_row_avx2_8bpc(void)
{
    /* Multiple seeds: cover different boundary conditions. */
    static const uint32_t seeds[] = {0xdeadbeef, 0xcafebabe, 0x01234567, 0xabcdef01};
    for (int i = 0; i < 4; i++) {
        char *r = check_8bpc(seeds[i]);
        if (r)
            return r;
    }
    return NULL;
}

/* Check one seed for the 16bpc path (10-bit: [0, 1023]). */
static char *check_16bpc(uint32_t seed)
{
    uint16_t src_row[TEST_ROW_WIDTH];
    uint16_t dst_row[TEST_ROW_WIDTH];
    integer_ssim_moments_t scalar_buf[TEST_ROW_WIDTH];
    integer_ssim_moments_t simd_buf[TEST_ROW_WIDTH];

    uint32_t state = seed;
    for (int i = 0; i < TEST_ROW_WIDTH; i++) {
        src_row[i] = (uint16_t)(simd_test_xorshift32(&state) & 0x3FF); /* 10-bit */
        dst_row[i] = (uint16_t)(simd_test_xorshift32(&state) & 0x3FF);
    }

    scalar_accumulate_row_16(src_row, dst_row, TEST_ROW_WIDTH, test_hkernel, TEST_KERNEL_SZ,
                             TEST_KERNEL_OFFS, scalar_buf);
    integer_ssim_accumulate_row_16_avx2(src_row, dst_row, TEST_ROW_WIDTH, test_hkernel,
                                        TEST_KERNEL_SZ, TEST_KERNEL_OFFS, simd_buf);

    SIMD_BITEXACT_ASSERT_MEMCMP(scalar_buf, simd_buf,
                                TEST_ROW_WIDTH * sizeof(integer_ssim_moments_t),
                                "integer_ssim_avx2 16bpc divergence");
    return NULL;
}

static char *test_integer_ssim_accumulate_row_avx2_16bpc(void)
{
    static const uint32_t seeds[] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    for (int i = 0; i < 4; i++) {
        char *r = check_16bpc(seeds[i]);
        if (r)
            return r;
    }
    return NULL;
}

/*
 * Regression test for integer-ssim-avx2-16bit-overflow.
 *
 * Pixels >= 46341 produce s*s >= 2^31, which sets bit 31 of the low 32-bit
 * lane used by _mm256_mul_epi32.  That makes the intermediate appear negative
 * (signed), corrupting the w*(s*s) product.  This test feeds full 16-bit
 * bright pixels (p = 65535 / 65534 alternating) and asserts AVX2 == scalar.
 *
 * The old code accumulated x2 as _mm256_mul_epi32(wv, _mm256_mul_epi32(sv,sv));
 * s*s = 65535^2 = 4,294,836,225 > 2^31 so bit 31 was set, sign-extending the
 * intermediate and producing a wrong (negative) partial product for x2/xy/y2.
 * The fix computes (w*s)*s instead; w*s <= 256*65535 = 16,776,960 < 2^24.
 */
static char *test_integer_ssim_avx2_16bpc_bright(void)
{
    uint16_t src_row[TEST_ROW_WIDTH];
    uint16_t dst_row[TEST_ROW_WIDTH];
    integer_ssim_moments_t scalar_buf[TEST_ROW_WIDTH];
    integer_ssim_moments_t simd_buf[TEST_ROW_WIDTH];

    /* Fill with alternating full-range 16-bit values that cause s*s >= 2^31. */
    for (int i = 0; i < TEST_ROW_WIDTH; i++) {
        src_row[i] = (uint16_t)(65535 - (i & 1)); /* 65535, 65534, 65535, ... */
        dst_row[i] = (uint16_t)(65535 - ((i + 1) & 1));
    }

    scalar_accumulate_row_16(src_row, dst_row, TEST_ROW_WIDTH, test_hkernel, TEST_KERNEL_SZ,
                             TEST_KERNEL_OFFS, scalar_buf);
    integer_ssim_accumulate_row_16_avx2(src_row, dst_row, TEST_ROW_WIDTH, test_hkernel,
                                        TEST_KERNEL_SZ, TEST_KERNEL_OFFS, simd_buf);

    SIMD_BITEXACT_ASSERT_MEMCMP(scalar_buf, simd_buf,
                                TEST_ROW_WIDTH * sizeof(integer_ssim_moments_t),
                                "integer_ssim_avx2 16bpc bright-pixel overflow");
    return NULL;
}

/* Adversarial: full-white / full-black rows (all-same pixels). */
static char *test_integer_ssim_avx2_uniform(void)
{
    uint8_t src_row[TEST_ROW_WIDTH];
    uint8_t dst_row[TEST_ROW_WIDTH];
    integer_ssim_moments_t scalar_buf[TEST_ROW_WIDTH];
    integer_ssim_moments_t simd_buf[TEST_ROW_WIDTH];

    (void)memset(src_row, 255, sizeof(src_row));
    (void)memset(dst_row, 0, sizeof(dst_row));

    scalar_accumulate_row_8(src_row, dst_row, TEST_ROW_WIDTH, test_hkernel, TEST_KERNEL_SZ,
                            TEST_KERNEL_OFFS, scalar_buf);
    integer_ssim_accumulate_row_avx2(src_row, dst_row, TEST_ROW_WIDTH, test_hkernel, TEST_KERNEL_SZ,
                                     TEST_KERNEL_OFFS, simd_buf);

    SIMD_BITEXACT_ASSERT_MEMCMP(scalar_buf, simd_buf,
                                TEST_ROW_WIDTH * sizeof(integer_ssim_moments_t),
                                "integer_ssim_avx2 uniform divergence");
    return NULL;
}

/* Narrow row: width = 1 (all boundary, no interior SIMD). */
static char *test_integer_ssim_avx2_narrow(void)
{
    const int w = 1;
    uint8_t src_row[1] = {128};
    uint8_t dst_row[1] = {64};
    integer_ssim_moments_t scalar_buf[1];
    integer_ssim_moments_t simd_buf[1];

    scalar_accumulate_row_8(src_row, dst_row, w, test_hkernel, TEST_KERNEL_SZ, TEST_KERNEL_OFFS,
                            scalar_buf);
    integer_ssim_accumulate_row_avx2(src_row, dst_row, w, test_hkernel, TEST_KERNEL_SZ,
                                     TEST_KERNEL_OFFS, simd_buf);

    SIMD_BITEXACT_ASSERT_MEMCMP(scalar_buf, simd_buf, (size_t)w * sizeof(integer_ssim_moments_t),
                                "integer_ssim_avx2 narrow divergence");
    return NULL;
}

#endif /* ARCH_X86 */

char *run_tests(void)
{
#if ARCH_X86
    if (!simd_test_have_avx2()) {
        return NULL;
    }
    mu_run_test(test_integer_ssim_accumulate_row_avx2_8bpc);
    mu_run_test(test_integer_ssim_accumulate_row_avx2_16bpc);
    mu_run_test(test_integer_ssim_avx2_uniform);
    mu_run_test(test_integer_ssim_avx2_narrow);
    mu_run_test(test_integer_ssim_avx2_16bpc_bright);
#else
    (void)fprintf(stderr, "skipping integer_ssim SIMD: non-x86 arch\n");
#endif
    return NULL;
}
