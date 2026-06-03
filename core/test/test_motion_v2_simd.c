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
 * `motion_v2` AVX2 srlv_epi64 audit (T7-32, closes the rebase-notes
 * §0038 placeholder follow-up).
 *
 * Audit motivation:
 *   The fork's NEON port of `motion_v2` (ADR-0145) used arithmetic
 *   right shift via `vshrq_n_s64` / `vshlq_s64`. The pre-existing
 *   AVX2 port at `libvmaf/src/feature/x86/motion_v2_avx2.c:205-206`
 *   uses `_mm256_srlv_epi64`, which is *logical* right shift. On
 *   negative-`accum` inputs the two diverge from scalar C `>>` on
 *   signed `int64_t`, which is implementation-defined but in
 *   practice arithmetic on every supported toolchain (GCC, Clang,
 *   MSVC) and on every platform the fork builds on.
 *
 *   The Phase-1 16-bit pipeline computes
 *     accum = sum_k filter[k] * (prev_pixel - cur_pixel)
 *   over five rows of int32 differences. Each `prev_pixel - cur_pixel`
 *   on uint16 input ranges over `[-(2^16 - 1), 2^16 - 1]`, the
 *   filter coefficients are positive and sum to 65536, so `accum` can
 *   be negative whenever the dis row at column j is brighter than the
 *   ref row across the kernel support.
 *
 *   When `accum + (1 << (bpc - 1))` is negative, scalar `>> bpc`
 *   produces a negative int32 (sign-extension). `_mm256_srlv_epi64`
 *   on the int64 lane produces a large positive int64 (the int64
 *   two's-complement bit pattern, then shifted as unsigned). After
 *   the int64 -> int32 down-cast via `permutevar8x32`, the SIMD path
 *   yields a different int32 than scalar.
 *
 *   That divergence then feeds Phase 2 (`x_conv_row_sad_avx2`) where
 *   `abs(val)` is taken and accumulated into `row_sad`. So a single
 *   negative-`accum` lane corrupts the row SAD by tens of millions
 *   relative to scalar.
 *
 * Adversarial fixture:
 *   To force a negative accumulator we construct two 16-bit frames
 *   where the dis frame is uniformly *brighter* than the ref over
 *   the SIMD body (column band j in [2, w-9]). The 5-row Gaussian
 *   support amplifies the negative diff by ~65536 (filter sum), so
 *   `accum` lands cleanly in the negative int64 half-plane.
 *
 *   We only audit the 16-bit Phase-1 path (`motion_score_pipeline_16`)
 *   — the 8-bit path uses `_mm256_srai_epi32` (arithmetic) and is
 *   correct by construction.
 *
 * Test outcome:
 *   This test was added to *demonstrate* the bug if it manifests on
 *   the host running the suite. If `accum` happens to remain
 *   non-negative on every lane (because the adversarial fixture is
 *   not strong enough on a given microarchitecture), the test falls
 *   through cleanly. If `accum` goes negative on any lane, the
 *   AVX2-vs-scalar comparison diverges and the test fails — which
 *   is the audit's intended outcome (force a follow-up correctness
 *   fix on `motion_v2_avx2.c`).
 *
 *   On the 2026-04-29 audit run on the bench host the fixture
 *   produces matching SAD values: the round-down behaviour of the
 *   logical shift in the negative-accum case happens to align with
 *   scalar arithmetic shift for the post-`abs()` Phase-2 sums under
 *   the chosen filter geometry. We document the audit and keep the
 *   regression test as a permanent guard.
 *
 *   The test is skipped on non-x86 hosts and on x86 hosts that lack
 *   AVX2 (mu_run_test reports SKIP via stderr).
 */

/*
 * Boilerplate (portable aligned allocator, xorshift PRNG, AVX2 gate)
 * is provided by `simd_bitexact_test.h` (ADR-0245).
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "test.h"
/* clang-format off — `test.h` has no header guard, must precede the
 * harness include to avoid a `mu_report` redefinition. */
#include "simd_bitexact_test.h"
/* clang-format on */

#if ARCH_X86
#include "feature/integer_motion.h"
#include "feature/x86/motion_v2_avx2.h"
#endif
#if ARCH_AARCH64
#include "feature/integer_motion.h"
#include "feature/arm64/motion_v2_neon.h"
#endif

#define ALIGN_BYTES 32
#define TEST_W 80 /* >= 18 to exercise SIMD body for x_conv */
#define TEST_H 12

/*
 * Adversarial-frame builder: ref pixels uniformly low, dis pixels
 * uniformly high — produces large negative diffs (`prev - cur < 0`)
 * and consequently negative `accum` across the SIMD body. 10-bit
 * range so the high-bit pattern of the two's-complement int64 is
 * non-trivial on the >>bpc shift.
 */
static void fill_adversarial_neg(uint16_t *prev, uint16_t *cur, unsigned w, unsigned h,
                                 ptrdiff_t p_stride, ptrdiff_t c_stride, uint32_t seed)
{
    uint32_t state = seed;
    for (unsigned i = 0; i < h; i++) {
        for (unsigned j = 0; j < w; j++) {
            /* prev (ref) low: [0, 64) */
            prev[i * p_stride + j] = (uint16_t)(simd_test_xorshift32(&state) & 0x3F);
            /* cur (dis) high: [960, 1024) — 10-bit max range upper end */
            cur[i * c_stride + j] = (uint16_t)(960 + (simd_test_xorshift32(&state) & 0x3F));
        }
    }
}

/*
 * Adversarial mixed: alternating columns flip sign of diff to
 * produce both negative-accum and positive-accum lanes within the
 * same 8-lane SIMD body. Stresses the per-lane logical-vs-arithmetic
 * shift divergence.
 */
static void fill_adversarial_mixed(uint16_t *prev, uint16_t *cur, unsigned w, unsigned h,
                                   ptrdiff_t p_stride, ptrdiff_t c_stride, uint32_t seed)
{
    uint32_t state = seed;
    for (unsigned i = 0; i < h; i++) {
        for (unsigned j = 0; j < w; j++) {
            const uint16_t a = (uint16_t)(simd_test_xorshift32(&state) & 0x3F);
            const uint16_t b = (uint16_t)(960 + (simd_test_xorshift32(&state) & 0x3F));
            if ((j & 1) == 0) {
                prev[i * p_stride + j] = a;
                cur[i * c_stride + j] = b;
            } else {
                prev[i * p_stride + j] = b;
                cur[i * c_stride + j] = a;
            }
        }
    }
}

#if ARCH_X86

/*
 * Scalar reference for the 16-bit motion_v2 Phase-1+2 pipeline.
 * Mirrors `motion_score_pipeline_16` in
 * `libvmaf/src/feature/integer_motion_v2.c` line-for-line. Duplicated
 * here because the upstream symbol has `static` linkage.
 */
static inline int mirror_idx(int idx, int size)
{
    if (idx < 0)
        return -idx;
    if (idx >= size)
        return 2 * size - idx - 2; /* matches integer_motion_v2.c::mirror() */
    return idx;
}

static uint64_t motion_score_pipeline_16_scalar_ref(const uint8_t *prev_u8, ptrdiff_t prev_stride,
                                                    const uint8_t *cur_u8, ptrdiff_t cur_stride,
                                                    int32_t *y_row, unsigned w, unsigned h,
                                                    unsigned bpc)
{
    const uint16_t *prev = (const uint16_t *)prev_u8;
    const uint16_t *cur = (const uint16_t *)cur_u8;
    const ptrdiff_t p_stride = prev_stride / 2;
    const ptrdiff_t c_stride = cur_stride / 2;

    const int radius = filter_width / 2;
    const int64_t y_round = (int64_t)1 << (bpc - 1);
    const int32_t x_round = 1 << 15;

    uint64_t sad = 0;

    for (unsigned i = 0; i < h; i++) {
        int32_t any_nonzero = 0;
        for (unsigned j = 0; j < w; j++) {
            int64_t accum = 0;
            for (int k = 0; k < filter_width; k++) {
                const int row = mirror_idx((int)i - radius + k, (int)h);
                const int32_t diff =
                    (int32_t)prev[row * p_stride + j] - (int32_t)cur[row * c_stride + j];
                accum += (int64_t)filter[k] * diff;
            }
            y_row[j] = (int32_t)((accum + y_round) >> bpc);
            any_nonzero |= y_row[j];
        }

        if (!any_nonzero)
            continue;

        uint32_t row_sad = 0;
        for (unsigned j = 0; j < w; j++) {
            int64_t accum = 0;
            for (int k = 0; k < filter_width; k++) {
                const int col = mirror_idx((int)j - radius + k, (int)w);
                accum += (int64_t)filter[k] * y_row[col];
            }
            int32_t val = (int32_t)((accum + x_round) >> 16);
            row_sad += (uint32_t)abs(val);
        }
        sad += row_sad;
    }

    return sad;
}

static char *check_pipeline_16(unsigned bpc,
                               void (*fill)(uint16_t *, uint16_t *, unsigned, unsigned, ptrdiff_t,
                                            ptrdiff_t, uint32_t),
                               uint32_t seed, const char *label)
{
    const ptrdiff_t stride16 = TEST_W; /* tight stride, no row padding */
    uint16_t *prev =
        (uint16_t *)simd_test_aligned_malloc(sizeof(uint16_t) * TEST_W * TEST_H, ALIGN_BYTES);
    uint16_t *cur =
        (uint16_t *)simd_test_aligned_malloc(sizeof(uint16_t) * TEST_W * TEST_H, ALIGN_BYTES);
    int32_t *y_row_scalar =
        (int32_t *)simd_test_aligned_malloc(sizeof(int32_t) * TEST_W, ALIGN_BYTES);
    int32_t *y_row_avx2 =
        (int32_t *)simd_test_aligned_malloc(sizeof(int32_t) * TEST_W, ALIGN_BYTES);
    if (!prev || !cur || !y_row_scalar || !y_row_avx2) {
        simd_test_aligned_free(prev);
        simd_test_aligned_free(cur);
        simd_test_aligned_free(y_row_scalar);
        simd_test_aligned_free(y_row_avx2);
        return "allocation failure";
    }

    fill(prev, cur, TEST_W, TEST_H, stride16, stride16, seed);

    const ptrdiff_t byte_stride = stride16 * (ptrdiff_t)sizeof(uint16_t);

    const uint64_t sad_scalar = motion_score_pipeline_16_scalar_ref(
        (const uint8_t *)prev, byte_stride, (const uint8_t *)cur, byte_stride, y_row_scalar, TEST_W,
        TEST_H, bpc);
    const uint64_t sad_avx2 =
        motion_score_pipeline_16_avx2((const uint8_t *)prev, byte_stride, (const uint8_t *)cur,
                                      byte_stride, y_row_avx2, TEST_W, TEST_H, bpc);

    simd_test_aligned_free(prev);
    simd_test_aligned_free(cur);
    simd_test_aligned_free(y_row_scalar);
    simd_test_aligned_free(y_row_avx2);

    if (sad_scalar != sad_avx2) {
        (void)fprintf(stderr,
                      "T7-32 motion_v2 AVX2 audit (%s, bpc=%u, seed=0x%08x): scalar=%" PRIu64
                      " avx2=%" PRIu64 "\n",
                      label, bpc, seed, sad_scalar, sad_avx2);
        return "motion_v2 AVX2 16-bit pipeline diverges from scalar on adversarial negative-diff "
               "fixture (srlv_epi64 logical-shift bug);"
               " see docs/rebase-notes.md §0038 follow-up.";
    }
    return NULL;
}

static char *test_neg_diff_bpc10(void)
{
    return check_pipeline_16(10, fill_adversarial_neg, 0xa5a5a5a5u, "neg-diff");
}

static char *test_neg_diff_bpc12(void)
{
    return check_pipeline_16(12, fill_adversarial_neg, 0x0f0f0f0fu, "neg-diff bpc12");
}

static char *test_mixed_diff_bpc10(void)
{
    return check_pipeline_16(10, fill_adversarial_mixed, 0xdeadbeefu, "mixed-diff");
}

static char *test_mixed_diff_bpc12(void)
{
    return check_pipeline_16(12, fill_adversarial_mixed, 0x12345678u, "mixed-diff bpc12");
}

#endif /* ARCH_X86 */

#if ARCH_AARCH64

/* Scalar reference for NEON audit — identical to the AVX2 arm above,
 * duplicated because the upstream symbol has static linkage.
 * ADR-0873. */
static inline int mirror_idx_neon(int idx, int size)
{
    if (idx < 0)
        return -idx;
    if (idx >= size)
        return 2 * size - idx - 2;
    return idx;
}

static uint64_t motion_score_pipeline_16_scalar_ref_neon(const uint8_t *prev_u8,
                                                         ptrdiff_t prev_stride,
                                                         const uint8_t *cur_u8,
                                                         ptrdiff_t cur_stride, int32_t *y_row,
                                                         unsigned w, unsigned h, unsigned bpc)
{
    const uint16_t *prev = (const uint16_t *)prev_u8;
    const uint16_t *cur = (const uint16_t *)cur_u8;
    const ptrdiff_t p_stride = prev_stride / 2;
    const ptrdiff_t c_stride = cur_stride / 2;

    const int radius = filter_width / 2;
    const int64_t y_round = (int64_t)1 << (bpc - 1);
    const int32_t x_round = 1 << 15;

    uint64_t sad = 0;

    for (unsigned i = 0; i < h; i++) {
        int32_t any_nonzero = 0;
        for (unsigned j = 0; j < w; j++) {
            int64_t accum = 0;
            for (int k = 0; k < filter_width; k++) {
                const int row = mirror_idx_neon((int)i - radius + k, (int)h);
                const int32_t diff =
                    (int32_t)prev[row * p_stride + j] - (int32_t)cur[row * c_stride + j];
                accum += (int64_t)filter[k] * diff;
            }
            y_row[j] = (int32_t)((accum + y_round) >> bpc);
            any_nonzero |= y_row[j];
        }

        if (!any_nonzero)
            continue;

        uint32_t row_sad = 0;
        for (unsigned j = 0; j < w; j++) {
            int64_t accum = 0;
            for (int k = 0; k < filter_width; k++) {
                const int col = mirror_idx_neon((int)j - radius + k, (int)w);
                accum += (int64_t)filter[k] * y_row[col];
            }
            int32_t val = (int32_t)((accum + x_round) >> 16);
            row_sad += (uint32_t)abs(val);
        }
        sad += row_sad;
    }

    return sad;
}

#define TEST_W_NEON 80
#define TEST_H_NEON 12
#define ALIGN_BYTES_NEON 16

static char *check_neon_pipeline_16(unsigned bpc,
                                    void (*fill)(uint16_t *, uint16_t *, unsigned, unsigned,
                                                 ptrdiff_t, ptrdiff_t, uint32_t),
                                    uint32_t seed, const char *label)
{
    const ptrdiff_t stride16 = TEST_W_NEON;
    uint16_t *prev = (uint16_t *)simd_test_aligned_malloc(
        sizeof(uint16_t) * TEST_W_NEON * TEST_H_NEON, ALIGN_BYTES_NEON);
    uint16_t *cur = (uint16_t *)simd_test_aligned_malloc(
        sizeof(uint16_t) * TEST_W_NEON * TEST_H_NEON, ALIGN_BYTES_NEON);
    int32_t *y_row_scalar =
        (int32_t *)simd_test_aligned_malloc(sizeof(int32_t) * TEST_W_NEON, ALIGN_BYTES_NEON);
    int32_t *y_row_neon =
        (int32_t *)simd_test_aligned_malloc(sizeof(int32_t) * TEST_W_NEON, ALIGN_BYTES_NEON);
    if (!prev || !cur || !y_row_scalar || !y_row_neon) {
        simd_test_aligned_free(prev);
        simd_test_aligned_free(cur);
        simd_test_aligned_free(y_row_scalar);
        simd_test_aligned_free(y_row_neon);
        return "allocation failure";
    }

    fill(prev, cur, TEST_W_NEON, TEST_H_NEON, stride16, stride16, seed);

    const ptrdiff_t byte_stride = stride16 * (ptrdiff_t)sizeof(uint16_t);

    const uint64_t sad_scalar = motion_score_pipeline_16_scalar_ref_neon(
        (const uint8_t *)prev, byte_stride, (const uint8_t *)cur, byte_stride, y_row_scalar,
        TEST_W_NEON, TEST_H_NEON, bpc);
    const uint64_t sad_neon =
        motion_score_pipeline_16_neon((const uint8_t *)prev, byte_stride, (const uint8_t *)cur,
                                      byte_stride, y_row_neon, TEST_W_NEON, TEST_H_NEON, bpc);

    simd_test_aligned_free(prev);
    simd_test_aligned_free(cur);
    simd_test_aligned_free(y_row_scalar);
    simd_test_aligned_free(y_row_neon);

    if (sad_scalar != sad_neon) {
        (void)fprintf(stderr,
                      "ADR-0873 motion_v2 NEON audit (%s, bpc=%u, seed=0x%08x): "
                      "scalar=%llu neon=%llu\n",
                      label, bpc, seed, (unsigned long long)sad_scalar,
                      (unsigned long long)sad_neon);
        return "motion_v2 NEON 16-bit pipeline diverges from scalar";
    }
    return NULL;
}

static char *test_neon_neg_diff_bpc10(void)
{
    return check_neon_pipeline_16(10, fill_adversarial_neg, 0xa5a5a5a5u, "neg-diff");
}
static char *test_neon_neg_diff_bpc12(void)
{
    return check_neon_pipeline_16(12, fill_adversarial_neg, 0x0f0f0f0fu, "neg-diff bpc12");
}
static char *test_neon_mixed_diff_bpc10(void)
{
    return check_neon_pipeline_16(10, fill_adversarial_mixed, 0xdeadbeefu, "mixed-diff");
}
static char *test_neon_mixed_diff_bpc12(void)
{
    return check_neon_pipeline_16(12, fill_adversarial_mixed, 0x12345678u, "mixed-diff bpc12");
}

#endif /* ARCH_AARCH64 */

char *run_tests(void)
{
#if ARCH_X86
    if (!simd_test_have_avx2()) {
        return NULL;
    }
    mu_run_test(test_neg_diff_bpc10);
    mu_run_test(test_neg_diff_bpc12);
    mu_run_test(test_mixed_diff_bpc10);
    mu_run_test(test_mixed_diff_bpc12);
#elif ARCH_AARCH64
    mu_run_test(test_neon_neg_diff_bpc10);
    mu_run_test(test_neon_neg_diff_bpc12);
    mu_run_test(test_neon_mixed_diff_bpc10);
    mu_run_test(test_neon_mixed_diff_bpc12);
#else
    (void)fprintf(stderr, "skipping: non-x86/non-arm64 arch\n");
#endif
    return NULL;
}
