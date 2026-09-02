/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * NEON-vs-scalar bit-exactness for the float-ADM DWT2 kernel
 * (`float_adm_dwt2_neon`, core/src/feature/arm64/float_adm_dwt2_neon.c).
 *
 * Sibling of test_adm_dwt2_neon.c, which covers the integer twin
 * (`adm_dwt2_8_neon`).  That kernel shipped three separate defects that no
 * test caught: a dropped filter tap (ADR-1057), a vector loop whose 16-column
 * stride did not divide the widths the dispatcher admitted (with no scalar
 * tail, so the last columns of the scratch row kept the previous row's
 * residue), and a horizontal pass that used flat pointer arithmetic instead
 * of the `ind_x` mirror table.  Nothing covered the float kernel either; this
 * test closes that gap.
 *
 * The reference is the *production* scalar `adm_dwt2_s()` from adm_tools.c,
 * linked out of `libvmaf_feature_static_lib`, rather than a transcription.
 * Both TUs come from the same build — `adm_dwt2_s` with its function-scoped
 * contraction guard, the NEON kernel with the `arm64_adm_dwt2_neon_lib`
 * `-ffp-contract=off` carve-out and explicit split operations — so the
 * comparison exercises the exact object code that ships, including the
 * FP-contraction contract from ADR-1057. A transcribed reference compiled
 * with the test TU's own flags could silently misrepresent that contract.
 *
 * What is asserted, per geometry:
 *   1. Bit-exact equality of all four subbands over the whole valid region.
 *      Bit patterns, not `==`, so a +0/-0 divergence cannot slip through.
 *   2. The NEON kernel writes nothing outside `[0, h_half) x [0, w_half)` --
 *      the guard against a vector store running past the row into the stride
 *      padding.
 *
 * Geometry coverage is chosen against the structural risks:
 *   - `adm_dwt2_dispatch()` in adm.c routes to NEON for *every* width, with
 *     no `w % N` guard, while the vertical pass steps 4 columns.  Widths with
 *     `w % 4` in {0,1,2,3} and widths below the vector width are all covered,
 *     both in the hand-picked list and in an exhaustive small sweep, so a
 *     missing or short scalar tail surfaces as a mismatch in the last
 *     `w % 4` columns of the scratch row.
 *   - The horizontal pass must consult `ind_x`, whose first and last entries
 *     are mirrored rather than sequential.  Odd widths, odd heights and the
 *     `j == 0` / `i == 0` rows are exercised on every geometry.
 *   - Strides are padded independently of the width, so a kernel that assumed
 *     `stride == w` would be caught.
 *
 * Negative controls run while writing this test (each reverted afterwards)
 * confirmed the assertions bite: disabling the vertical scalar tail, swapping
 * `ind_x` for clamped flat arithmetic, and introducing an explicit FMA each
 * produced a failure with the predicted mismatch footprint.
 */

#include <stdio.h>

#include "config.h"
#include "test.h"

#include "feature/adm_tools.h"

#if ARCH_AARCH64
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "feature/arm64/float_adm_neon.h"

/* Sentinel poison for cells neither kernel is allowed to touch: a quiet-NaN
 * payload that is trivially recognisable in a bit-pattern dump. */
#define POISON_BITS 0x7FC0DEADu

static uint32_t float_bits(float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

static void fill_poison(float *p, size_t n)
{
    float v;
    uint32_t bits = POISON_BITS;
    memcpy(&v, &bits, sizeof(v));
    for (size_t k = 0; k < n; ++k)
        p[k] = v;
}

/* xorshift32 -> pixel-range float with a fractional part.  Integral pixel
 * values hide ordering and tap bugs behind exact arithmetic far too often, so
 * the samples deliberately do not land on representable-sum boundaries. */
static float next_sample(uint32_t *seed)
{
    uint32_t s = *seed;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    *seed = s;
    return (float)(s & 0xFFFFu) * (255.0f / 65535.0f);
}

/*
 * Run one geometry through both kernels and count divergent cells.
 *
 * `verbose` controls whether the first few mismatches are dumped; the
 * exhaustive sweep keeps it off until it finds something.  Returns a mu_assert
 * message on infrastructure failure (allocation / scalar error), NULL
 * otherwise, with the mismatch count in *out_mismatches.
 */
static char *compare_geometry(int w, int h, int src_pad, int dst_pad, int signed_zero_case,
                              int verbose, int *out_mismatches)
{
    const int w_half = (w + 1) / 2, h_half = (h + 1) / 2;
    const int src_px_stride = w + src_pad;
    const int dst_px_stride = w_half + dst_pad;
    const size_t dst_cells = (size_t)h_half * (size_t)dst_px_stride;
    static const char *const names[4] = {"band_a", "band_v", "band_h", "band_d"};

    float *src = NULL;
    float *bands[8] = {0};
    int *iy[4] = {0}, *ix[4] = {0};
    adm_dwt_band_t_s ref_band, simd_band;
    uint32_t seed = 0x5eed0000u ^ (uint32_t)(w * 131 + h * 7 + 1);
    char *msg = NULL;
    int mismatches = 0, tail_col_mismatches = 0, last_col_mismatches = 0;

    *out_mismatches = 0;

    src = malloc(sizeof(float) * (size_t)h * (size_t)src_px_stride);
    for (int k = 0; k < 4; ++k) {
        iy[k] = calloc((size_t)h_half + 4, sizeof(int));
        ix[k] = calloc((size_t)w_half + 4, sizeof(int));
    }
    for (int k = 0; k < 8; ++k)
        bands[k] = malloc(sizeof(float) * dst_cells);

    if (!src) {
        msg = "allocation failed";
        goto out;
    }
    for (int k = 0; k < 4; ++k) {
        if (!iy[k] || !ix[k]) {
            msg = "allocation failed";
            goto out;
        }
    }
    for (int k = 0; k < 8; ++k) {
        if (!bands[k]) {
            msg = "allocation failed";
            goto out;
        }
        fill_poison(bands[k], dst_cells);
    }

    ref_band.band_a = bands[0];
    ref_band.band_v = bands[1];
    ref_band.band_h = bands[2];
    ref_band.band_d = bands[3];
    simd_band.band_a = bands[4];
    simd_band.band_v = bands[5];
    simd_band.band_h = bands[6];
    simd_band.band_d = bands[7];

    /* Fill the stride padding too: a kernel that read past `w` would then be
     * consuming real data rather than whatever malloc left behind, which keeps
     * the failure deterministic instead of run-to-run noise. */
    for (int i = 0; i < h; ++i) {
        for (int j = 0; j < src_px_stride; ++j)
            src[(size_t)i * src_px_stride + j] = signed_zero_case ? 0.0f : next_sample(&seed);
    }

    if (signed_zero_case) {
        /* At output (1, 1), the mirror tables select source rows/columns
         * 1, 2, 3, 4.  For columns 1..3, make every low-pass vertical
         * product -0; leave column 4 at +0.  A kernel that initializes an
         * accumulator with tap 0 instead of scalar's +0 therefore produces
         * vertical signs {-0,-0,-0,+0}, then a low-pass horizontal -0.
         * The scalar +0 plus four products remains +0 at both stages. */
        for (int j = 1; j <= 3; ++j) {
            src[(size_t)1 * src_px_stride + j] = -0.0f;
            src[(size_t)2 * src_px_stride + j] = -0.0f;
            src[(size_t)3 * src_px_stride + j] = -0.0f;
        }
    }

    dwt2_src_indices_filt_s(iy, ix, w, h);

    if (adm_dwt2_s(src, &ref_band, iy, ix, w, h, (int)(src_px_stride * sizeof(float)),
                   (int)(dst_px_stride * sizeof(float))) != 0) {
        msg = "adm_dwt2_s failed";
        goto out;
    }

    float_adm_dwt2_neon(src, &simd_band, iy, ix, w, h, (int)(src_px_stride * sizeof(float)),
                        (int)(dst_px_stride * sizeof(float)));

    for (int b = 0; b < 4; ++b) {
        for (int i = 0; i < h_half; ++i) {
            for (int j = 0; j < w_half; ++j) {
                const size_t idx = (size_t)i * (size_t)dst_px_stride + (size_t)j;
                const uint32_t want = float_bits(bands[b][idx]);
                const uint32_t got = float_bits(bands[b + 4][idx]);
                if (got == want)
                    continue;
                ++mismatches;
                /* Output columns whose horizontal support (2j-1 .. 2j+2)
                 * reaches into the region the vertical scalar tail owns. */
                if (2 * j + 2 >= (w & ~3))
                    ++tail_col_mismatches;
                if (j == w_half - 1)
                    ++last_col_mismatches;
                if (verbose && mismatches <= 8)
                    (void)fprintf(stderr,
                                  "  %dx%d (src_stride %d, dst_stride %d) %s[%d][%d]%s:"
                                  " scalar %.9g (0x%08x) != neon %.9g (0x%08x)\n",
                                  w, h, src_px_stride, dst_px_stride, names[b], i, j,
                                  (j == w_half - 1) ? " (last col)" : "", (double)bands[b][idx],
                                  want, (double)bands[b + 4][idx], got);
            }
        }
        /* Nothing may be written outside the valid region: the stride padding
         * must still hold the poison. */
        for (int i = 0; i < h_half; ++i) {
            for (int j = w_half; j < dst_px_stride; ++j) {
                const size_t idx = (size_t)i * (size_t)dst_px_stride + (size_t)j;
                if (float_bits(bands[b + 4][idx]) == POISON_BITS)
                    continue;
                ++mismatches;
                if (verbose)
                    (void)fprintf(stderr,
                                  "  %dx%d %s: neon wrote past the last valid column (%d)"
                                  " into the stride padding at row %d, col %d\n",
                                  w, h, names[b], w_half - 1, i, j);
            }
        }
    }

    if (verbose && mismatches)
        (void)fprintf(stderr,
                      "  %dx%d: %d mismatching cells (%d whose support touches the"
                      " vertical scalar tail, %d in the mirrored last column)\n",
                      w, h, mismatches, tail_col_mismatches, last_col_mismatches);

    *out_mismatches = mismatches;

out:
    for (int k = 0; k < 4; ++k) {
        free(iy[k]);
        free(ix[k]);
    }
    for (int k = 0; k < 8; ++k)
        free(bands[k]);
    free(src);
    return msg;
}
#endif /* ARCH_AARCH64 */

/* Hand-picked geometries: every `w % 4` residue, widths below the vector
 * width, both height parities, and independently padded strides. */
static char *test_float_adm_dwt2_neon_matches_scalar(void)
{
#if !ARCH_AARCH64
    return NULL; /* NEON kernel is aarch64-only. */
#else
    /* {w, h, src stride padding (px), dst stride padding (px)} */
    static const int geom[][4] = {
        {3, 5, 0, 0},
        {4, 4, 1, 2},
        {5, 7, 3, 1},
        {6, 3, 0, 3},
        {7, 9, 2, 0},
        {8, 8, 0, 0},
        {9, 6, 5, 2},
        {10, 11, 1, 1},
        {11, 4, 0, 5},
        {12, 12, 4, 4},
        {13, 15, 3, 2},
        {16, 16, 0, 0},
        {17, 5, 7, 3},
        {18, 13, 2, 1},
        {19, 21, 1, 0},
        {32, 17, 0, 0},
        {35, 33, 5, 6},
        {64, 3, 0, 0},
        {65, 8, 3, 3},
        {66, 7, 1, 2},
        {127, 9, 0, 1},
        {128, 10, 8, 8},
        /* The ADM scale pyramid for the 576x324 Netflix golden clip, which is
         * exactly the sequence of geometries adm.c hands the kernel there. */
        {576, 324, 0, 0},
        {288, 162, 6, 2},
        {144, 81, 4, 1},
        {72, 41, 2, 3},
        {36, 21, 1, 0},
    };

    for (size_t t = 0; t < sizeof(geom) / sizeof(geom[0]); ++t) {
        int mismatches = 0;
        char *msg =
            compare_geometry(geom[t][0], geom[t][1], geom[t][2], geom[t][3], 0, 1, &mismatches);
        mu_assert(msg, msg == NULL);
        mu_assert("float_adm_dwt2_neon diverges from adm_dwt2_s", mismatches == 0);
    }
    return NULL;
#endif
}

/* Exhaustive small sweep.  The dispatcher applies no width guard, so every
 * width in this range is one production can hand the kernel; enumerating them
 * removes any doubt that the hand-picked list happened to miss the residue
 * class where a stride/tail mismatch lives. */
static char *test_float_adm_dwt2_neon_geometry_sweep(void)
{
#if !ARCH_AARCH64
    return NULL;
#else
    for (int w = 2; w <= 40; ++w) {
        for (int h = 2; h <= 12; ++h) {
            int mismatches = 0;
            /* Vary the padding with the geometry so the sweep also covers
             * tight and padded strides for each residue class. */
            char *msg = compare_geometry(w, h, w % 5, h % 3, 0, 0, &mismatches);
            mu_assert(msg, msg == NULL);
            if (mismatches) {
                (void)fprintf(stderr, "  sweep: first divergence at %dx%d\n", w, h);
                /* Re-run verbosely so the failure report carries the detail. */
                (void)compare_geometry(w, h, w % 5, h % 3, 0, 1, &mismatches);
            }
            mu_assert("float_adm_dwt2_neon diverges from adm_dwt2_s in the geometry sweep",
                      mismatches == 0);
        }
    }
    return NULL;
#endif
}

/* The production scalar starts each four-tap sum at +0.  That first addition
 * is numerically redundant for ordinary finite samples but load-bearing for
 * IEEE-754 signed zero, so exercise it independently of the positive-only
 * pseudo-random geometry corpus above. */
static char *test_float_adm_dwt2_neon_preserves_signed_zero(void)
{
#if !ARCH_AARCH64
    return NULL;
#else
    int mismatches = 0;
    char *msg = compare_geometry(8, 6, 0, 0, 1, 1, &mismatches);
    mu_assert(msg, msg == NULL);
    mu_assert("float_adm_dwt2_neon diverges from scalar signed-zero contract", mismatches == 0);
    return NULL;
#endif
}

char *run_tests(void)
{
#if ARCH_AARCH64
    mu_run_test(test_float_adm_dwt2_neon_matches_scalar);
    mu_run_test(test_float_adm_dwt2_neon_geometry_sweep);
    mu_run_test(test_float_adm_dwt2_neon_preserves_signed_zero);
#else
    (void)fprintf(stderr, "skipping: non-aarch64 arch\n");
    (void)test_float_adm_dwt2_neon_matches_scalar;
    (void)test_float_adm_dwt2_neon_geometry_sweep;
    (void)test_float_adm_dwt2_neon_preserves_signed_zero;
#endif
    return NULL;
}
