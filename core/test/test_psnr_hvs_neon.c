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
 * Bit-exactness contract (ADR-0159 mirror, ADR-0160): the NEON
 * `od_bin_fdct8x8_neon` — the heart of the calc_psnrhvs NEON port —
 * produces byte-for-byte identical int32 DCT coefficients to the
 * scalar reference in libvmaf/src/feature/third_party/xiph/psnr_hvs.c
 * under FLT_EVAL_METHOD == 0 for every 12-bit input.
 *
 * Scope:
 *   - Single 8x8 integer DCT: random 12-bit input across several
 *     seeds; byte-equal coefficient output verified via `memcmp`.
 *
 * Full calc_psnrhvs end-to-end bit-exactness is validated through
 * CLI round-trip against the Netflix golden pair under scalar vs
 * NEON. See the deep-dive digest accompanying this PR.
 *
 * Boilerplate (xorshift PRNG, bit-exact memcmp assertion) is
 * provided by `simd_bitexact_test.h` (ADR-0245).
 */

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "test.h"
/* clang-format off — `test.h` has no header guard, must precede the
 * harness include to avoid a `mu_report` redefinition. */
#include "simd_bitexact_test.h"
/* clang-format on */

#if ARCH_AARCH64
#include "feature/arm64/psnr_hvs_neon.h"
#endif

#if ARCH_AARCH64

/* Scalar reference copy of `od_bin_fdct8x8` — duplicated here because
 * the upstream original has `static` linkage. Must track the upstream
 * scalar source line-for-line. */
typedef int32_t od_coeff;

#define OD_UNBIASED_RSHIFT32(_a, _b) (((int32_t)(((uint32_t)(_a) >> (32 - (_b))) + (_a))) >> (_b))
#define OD_DCT_RSHIFT(_a, _b) OD_UNBIASED_RSHIFT32(_a, _b)

// NOLINTNEXTLINE(readability-function-size) — load-bearing upstream scalar reference (ADR-0138 / ADR-0141 / ADR-0278).
static void ref_od_bin_fdct8(od_coeff y[8], const od_coeff *x, int xstride)
{
    const ptrdiff_t xs = (ptrdiff_t)xstride;
    int t0 = x[0 * xs];
    int t4 = x[1 * xs];
    int t2 = x[2 * xs];
    int t6 = x[3 * xs];
    int t7 = x[4 * xs];
    int t3 = x[5 * xs];
    int t5 = x[6 * xs];
    int t1 = x[7 * xs];
    int t1h;
    int t4h;
    int t6h;
    t1 = t0 - t1;
    t1h = OD_DCT_RSHIFT(t1, 1);
    t0 -= t1h;
    t4 += t5;
    t4h = OD_DCT_RSHIFT(t4, 1);
    t5 -= t4h;
    t3 = t2 - t3;
    t2 -= OD_DCT_RSHIFT(t3, 1);
    t6 += t7;
    t6h = OD_DCT_RSHIFT(t6, 1);
    t7 = t6h - t7;
    t0 += t6h;
    t6 = t0 - t6;
    t2 = t4h - t2;
    t4 = t2 - t4;
    t0 -= (t4 * 13573 + 16384) >> 15;
    t4 += (t0 * 11585 + 8192) >> 14;
    t0 -= (t4 * 13573 + 16384) >> 15;
    t6 -= (t2 * 21895 + 16384) >> 15;
    t2 += (t6 * 15137 + 8192) >> 14;
    t6 -= (t2 * 21895 + 16384) >> 15;
    t3 += (t5 * 19195 + 16384) >> 15;
    t5 += (t3 * 11585 + 8192) >> 14;
    t3 -= (t5 * 7489 + 4096) >> 13;
    t7 = OD_DCT_RSHIFT(t5, 1) - t7;
    t5 -= t7;
    t3 = t1h - t3;
    t1 -= t3;
    t7 += (t1 * 3227 + 16384) >> 15;
    t1 -= (t7 * 6393 + 16384) >> 15;
    t7 += (t1 * 3227 + 16384) >> 15;
    t5 += (t3 * 2485 + 4096) >> 13;
    t3 -= (t5 * 18205 + 16384) >> 15;
    t5 += (t3 * 2485 + 4096) >> 13;
    y[0] = t0;
    y[1] = t1;
    y[2] = t2;
    y[3] = t3;
    y[4] = t4;
    y[5] = t5;
    y[6] = t6;
    y[7] = t7;
}

static void ref_od_bin_fdct8x8(od_coeff *y, int ystride, const od_coeff *x, int xstride)
{
    const ptrdiff_t ys = (ptrdiff_t)ystride;
    od_coeff z[8 * 8];
    for (ptrdiff_t i = 0; i < 8; i++) {
        ref_od_bin_fdct8(z + 8 * i, x + i, xstride);
    }
    for (ptrdiff_t i = 0; i < 8; i++) {
        ref_od_bin_fdct8(y + ys * i, z + i, 8);
    }
}

static char *check_dct_block(uint32_t seed)
{
    od_coeff in[64];
    od_coeff out_scalar[64];
    od_coeff out_neon[64];
    simd_test_fill_random_i32_mod(in, 64, 4096, seed); /* 12-bit range */
    ref_od_bin_fdct8x8(out_scalar, 8, in, 8);
    od_bin_fdct8x8_neon(out_neon, 8, in, 8);
    SIMD_BITEXACT_ASSERT_MEMCMP(out_scalar, out_neon, sizeof(out_scalar),
                                "DCT NEON not bit-identical to scalar");
    return NULL;
}

static char *test_dct_seed_a(void)
{
    return check_dct_block(0xdeadbeefu);
}

static char *test_dct_seed_b(void)
{
    return check_dct_block(0x12345678u);
}

static char *test_dct_seed_c(void)
{
    return check_dct_block(0xabcdef01u);
}

/* Delta input — verifies DC coefficient handling. */
static char *test_dct_delta(void)
{
    od_coeff in[64] = {0};
    od_coeff out_scalar[64];
    od_coeff out_neon[64];
    in[0] = 1000;
    ref_od_bin_fdct8x8(out_scalar, 8, in, 8);
    od_bin_fdct8x8_neon(out_neon, 8, in, 8);
    SIMD_BITEXACT_ASSERT_MEMCMP(out_scalar, out_neon, sizeof(out_scalar),
                                "DCT NEON delta input not bit-identical");
    return NULL;
}

/* Constant-field input — DC-only output exercise. */
static char *test_dct_constant(void)
{
    od_coeff in[64];
    od_coeff out_scalar[64];
    od_coeff out_neon[64];
    for (int i = 0; i < 64; i++) {
        in[i] = 2048;
    }
    ref_od_bin_fdct8x8(out_scalar, 8, in, 8);
    od_bin_fdct8x8_neon(out_neon, 8, in, 8);
    SIMD_BITEXACT_ASSERT_MEMCMP(out_scalar, out_neon, sizeof(out_scalar),
                                "DCT NEON constant input not bit-identical");
    return NULL;
}

#endif /* ARCH_AARCH64 */

char *run_tests(void)
{
#if ARCH_AARCH64
    mu_run_test(test_dct_seed_a);
    mu_run_test(test_dct_seed_b);
    mu_run_test(test_dct_seed_c);
    mu_run_test(test_dct_delta);
    mu_run_test(test_dct_constant);
#else
    (void)fprintf(stderr, "skipping: non-aarch64 arch\n");
#endif
    return NULL;
}
