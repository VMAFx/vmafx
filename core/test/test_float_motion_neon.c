/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * NEON-vs-scalar bit-exactness for `float_sad_line_neon`
 * (core/src/feature/arm64/float_motion_neon.c).
 *
 * Why this test exists
 * --------------------
 * `float_sad_line_neon` had no unit test on any architecture.  The only
 * coverage was indirect, through the Netflix golden-data run on ARM, which
 * asserts a final VMAF score to six decimals — far too coarse to catch a
 * single-ULP reduction-order drift, and blind to any width the golden pair
 * does not happen to use (576 and 1920, both multiples of 4).
 *
 * ADR-1057 (`adm_dwt2_8_neon`, a dropped filter tap that reached master and
 * drifted the ARM golden) is the precedent: an untested NEON kernel is a
 * kernel whose contract is unverified.  The failure classes that bit that
 * kernel are exactly the ones probed here:
 *
 *   (a) vector-stride / admitted-width mismatch with no scalar tail.
 *       `float_sad_line_*` is dispatched unconditionally in
 *       `float_motion.c:init()` — there is *no* width guard at all, so the
 *       kernel must be correct for every `w >= 1`.  Widths 1..40 are swept
 *       so that every residue mod 4 is covered, including `w < 4` where the
 *       vector loop body never executes.
 *
 *   (b) boundary handling.  The kernel's only boundary is the `w % 4` tail;
 *       the sweep plus the large-width cases pin it.
 *
 *   (c) accumulator width and reduction order — the class the brief flags
 *       for float SIMD.  A horizontal `vaddvq_f32` / pairwise `vpaddq_f32`
 *       reduction, or a float32x4_t running accumulator drained once at the
 *       end, would each give a *different* float sum than the scalar's
 *       strictly sequential `accum += |d0|; accum += |d1|; ...`.  The
 *       adversarial fixtures below are built so that any such regrouping
 *       changes the result by far more than a ULP:
 *
 *         - `SPIKE_HEAD`: one 2^24-magnitude difference followed by 1.0f
 *           differences.  Sequential accumulation absorbs each 1.0f into
 *           2^24 and rounds to even (2^24 + 1 -> 2^24), so the sum stays
 *           pinned at 2^24 for a long run.  A tree/pairwise reduction sums
 *           the 1.0f terms among themselves first and *does* advance the
 *           total.  The two answers differ in the high bits, not the low.
 *         - `SPIKE_MID`: the same spike at `j == w/2`, so the absorbing
 *           regime starts mid-line and the `w % 4` remainder is summed while
 *           `accum` is already at 2^24.  This is what catches a kernel that
 *           folds its vector partial in *after* the scalar tail: the tail's
 *           1.0f terms then add to each other exactly (giving 1..3) instead
 *           of being absorbed, and the final `2^24 + 3` rounds to 2^24 + 4.
 *           (A spike placed *in* the remainder would not discriminate — the
 *           final two-operand fold is commutative.)
 *         - `DENORMAL`: differences in the 1e-40 range.  AArch64 Advanced
 *           SIMD and scalar FP share one FPCR, so flush-to-zero must be
 *           identical for both; a divergence here would mean the kernel had
 *           been built with a different FP mode than its caller.
 *
 * The reference is a line-for-line transcription of `float_sad_line_c` in
 * core/src/feature/float_motion.c, which is `static` and therefore not
 * linkable from a test TU.
 *
 * The second test pins the invariant that the comment above
 * `compute_motion_simd()` asserts but nothing checked: summing per-line
 * NEON SADs and dividing by `w * h` reproduces `vmaf_image_sad_c()` with
 * `motion_add_scale1 = 0` *bit-exactly*, on a padded stride.  That is the
 * value the motion / motion2 / motion3 features publish.
 *
 * Comparison is on the raw float bit pattern (`memcmp`), not `==`, so a
 * +0.0 / -0.0 or NaN-payload divergence cannot slip through.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "test.h"
/* clang-format off — `test.h` has no header guard and must precede the
 * harness include to avoid a `mu_report` redefinition. */
#include "simd_bitexact_test.h"
/* clang-format on */

#include "feature/motion.h"

#if ARCH_AARCH64
#include "feature/arm64/float_motion_neon.h"

/* Widest line the sweep allocates, plus room for the unaligned-start
 * offsets (0..3 floats) the sweep applies to both operands. */
#define MAX_W 2048
#define PAD 8

/* Fixture kinds — see the file banner for what each one pins down. */
enum sad_fixture {
    FIX_LUMA,       /* realistic post-blur luma range, both planes in [0,255] */
    FIX_SPIKE_HEAD, /* 2^24 difference at j == 0, 1.0f differences after */
    FIX_SPIKE_MID,  /* 2^24 difference at j == w/2, small ones on both sides */
    FIX_DENORMAL,   /* differences at ~1e-40, exercising flush-to-zero */
    FIX_MIXED,      /* wide dynamic range, alternating sign of the difference */
    FIX_COUNT
};

static const char *const fixture_names[FIX_COUNT] = {"luma", "spike_head", "spike_mid", "denormal",
                                                     "mixed"};

/* Line-for-line transcription of float_sad_line_c() in float_motion.c.
 * The accumulation order *is* the property under test, so this must not be
 * "simplified" into fabsf() or a compensated sum. */
static float ref_float_sad_line(const float *img1, const float *img2, int w)
{
    float accum = 0.0f;
    for (int j = 0; j < w; j++) {
        float diff = img1[j] - img2[j];
        accum += diff < 0 ? -diff : diff;
    }
    return accum;
}

static void fill_fixture(float *a, float *b, int w, enum sad_fixture kind, uint32_t seed)
{
    uint32_t state = seed ? seed : 1u;

    for (int j = 0; j < w; ++j) {
        const uint32_t r = simd_test_xorshift32(&state);

        switch (kind) {
        case FIX_LUMA:
            a[j] = (float)(r & 0xFFFFu) * (255.0f / 65535.0f);
            b[j] = (float)((r >> 16) & 0xFFFFu) * (255.0f / 65535.0f);
            break;
        case FIX_SPIKE_HEAD:
            a[j] = (j == 0) ? 16777216.0f : 1.0f; /* 2^24 */
            b[j] = 0.0f;
            break;
        case FIX_SPIKE_MID:
            a[j] = (j == w / 2) ? 16777216.0f : 1.0f;
            b[j] = 0.0f;
            break;
        case FIX_DENORMAL:
            a[j] = 1e-40f * (float)((r & 0x7u) + 1u);
            b[j] = (r & 0x8u) ? 0.0f : 1e-40f;
            break;
        case FIX_MIXED:
        default: {
            /* Exponents spread over ~2^-20 .. 2^20 so that regrouping the
             * partial sums cannot stay within a ULP of the sequential one. */
            const int e = (int)(r % 41u) - 20;
            float mag = 1.0f;
            for (int k = 0; k < (e < 0 ? -e : e); ++k)
                mag = (e < 0) ? mag * 0.5f : mag * 2.0f;
            a[j] = (r & 0x80000000u) ? mag : 0.0f;
            b[j] = (r & 0x80000000u) ? 0.0f : mag;
            break;
        }
        }
    }
}

static int floats_bit_equal(float x, float y)
{
    return memcmp(&x, &y, sizeof(float)) == 0;
}

static char *test_float_sad_line_neon_matches_scalar(void)
{
    /* No dispatch guard exists in float_motion.c:init() — the NEON kernel is
     * selected for every width — so every width must be exercised, not just
     * multiples of the 4-lane stride. */
    static const int wide[] = {41, 63, 64, 65, 127, 128, 129, 255, 256, 257, 576, 1919, 1920, 1921};
    const int n_wide = (int)(sizeof(wide) / sizeof(wide[0]));

    float *a_buf = simd_test_aligned_malloc((size_t)(MAX_W + PAD) * sizeof(float), 32);
    float *b_buf = simd_test_aligned_malloc((size_t)(MAX_W + PAD) * sizeof(float), 32);
    mu_assert("aligned allocation failed", a_buf != NULL && b_buf != NULL);

    int mismatches = 0;
    int per_fixture[FIX_COUNT] = {0};
    for (int kind = 0; kind < FIX_COUNT; ++kind) {
        for (int off = 0; off < 4; ++off) {
            for (int idx = 0; idx < 40 + n_wide; ++idx) {
                const int w = (idx < 40) ? idx + 1 : wide[idx - 40]; /* 1..40, then the wide set */
                float *a = a_buf + off;
                float *b = b_buf + off;

                fill_fixture(a, b, w, (enum sad_fixture)kind,
                             0x5eed0000u ^ (uint32_t)(w * 131 + kind * 7919 + off));

                const float expected = ref_float_sad_line(a, b, w);
                const float got = float_sad_line_neon(a, b, w);

                if (!floats_bit_equal(expected, got)) {
                    ++mismatches;
                    ++per_fixture[kind];
                    if (mismatches <= 8) {
                        uint32_t eb;
                        uint32_t gb;
                        memcpy(&eb, &expected, sizeof(eb));
                        memcpy(&gb, &got, sizeof(gb));
                        (void)fprintf(stderr,
                                      "  %s w=%d off=%d: scalar %.9g (0x%08x) != "
                                      "neon %.9g (0x%08x)\n",
                                      fixture_names[kind], w, off, (double)expected, eb,
                                      (double)got, gb);
                    }
                }
            }
        }
    }

    simd_test_aligned_free(a_buf);
    simd_test_aligned_free(b_buf);

    if (mismatches) {
        const int cases = FIX_COUNT * 4 * (40 + n_wide);
        (void)fprintf(stderr, "  %d / %d mismatching (fixture, width, offset) triples\n",
                      mismatches, cases);
        for (int kind = 0; kind < FIX_COUNT; ++kind)
            (void)fprintf(stderr, "    %-10s %d / %d\n", fixture_names[kind], per_fixture[kind],
                          4 * (40 + n_wide));
    }
    mu_assert("float_sad_line_neon diverges from the scalar reference", mismatches == 0);
    return NULL;
}

/* Pins the claim in the comment above compute_motion_simd(): the per-line
 * NEON SAD summed over rows and divided by w*h reproduces vmaf_image_sad_c()
 * bit-exactly.  vmaf_image_sad_c accumulates a per-line float and folds it
 * into the frame accumulator, so the line-granular SIMD split is only
 * equivalent while each line's own reduction stays sequential. */
static char *test_neon_frame_sad_matches_vmaf_image_sad_c(void)
{
    static const int geoms[][2] = {{16, 9}, {17, 5}, {35, 7}, {64, 8}, {66, 4}, {576, 12}};

    for (size_t g = 0; g < sizeof(geoms) / sizeof(geoms[0]); ++g) {
        const int w = geoms[g][0];
        const int h = geoms[g][1];
        /* Padded stride, as float_motion.c's ALIGN_CEIL(w * sizeof(float))
         * produces; the padding lanes must never be read. */
        const int stride = (w + 7) & ~7;

        float *a = simd_test_aligned_malloc((size_t)stride * h * sizeof(float), 32);
        float *b = simd_test_aligned_malloc((size_t)stride * h * sizeof(float), 32);
        mu_assert("aligned allocation failed", a != NULL && b != NULL);

        uint32_t state = 0xC0FFEE00u ^ (uint32_t)(w * 31 + h);
        for (int i = 0; i < stride * h; ++i) {
            a[i] = (float)(simd_test_xorshift32(&state) & 0xFFFFu) * (255.0f / 65535.0f);
            b[i] = (float)(simd_test_xorshift32(&state) & 0xFFFFu) * (255.0f / 65535.0f);
        }

        float accum = 0.0f;
        for (int i = 0; i < h; ++i)
            accum += float_sad_line_neon(a + (ptrdiff_t)i * stride, b + (ptrdiff_t)i * stride, w);
        const float simd_score = accum / (float)(w * h);
        const float scalar_score = vmaf_image_sad_c(a, b, w, h, stride, stride, 0);

        simd_test_aligned_free(a);
        simd_test_aligned_free(b);

        if (!floats_bit_equal(scalar_score, simd_score)) {
            (void)fprintf(stderr, "  %dx%d: vmaf_image_sad_c %.9g != neon frame SAD %.9g\n", w, h,
                          (double)scalar_score, (double)simd_score);
            return (char *)"NEON frame SAD diverges from vmaf_image_sad_c";
        }
    }
    return NULL;
}
#endif /* ARCH_AARCH64 */

char *run_tests(void)
{
#if ARCH_AARCH64
    mu_run_test(test_float_sad_line_neon_matches_scalar);
    mu_run_test(test_neon_frame_sad_matches_vmaf_image_sad_c);
#else
    (void)fprintf(stderr, "skipping: float_sad_line_neon is aarch64-only\n");
#endif
    return NULL;
}
