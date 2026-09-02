/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * NEON-vs-scalar bit-exactness for the integer-motion horizontal convolution.
 *
 * Coverage gap closed (companion to ADR-1057 / test_adm_dwt2_neon.c):
 * `core/src/feature/arm64/motion_neon.c` exports exactly one kernel,
 * `x_convolution_16_neon`, and until this file nothing anywhere in the tree
 * called it, compiled it into a test, or compared it against a reference.
 * The AVX-512 twin `x_convolution_16_avx512` got direct parity coverage in
 * ADR-0854 (`test_motion_avx512_parity.c`); the NEON twin did not. A dropped
 * tap, a missing scalar tail, or a mis-ordered coefficient would have been
 * invisible on every architecture.
 *
 * What is under test:
 *   - The 8-wide NEON body vs. the scalar 5-tap reference, bit-exact.
 *   - The scalar tail: the vector loop steps 8 columns and stops at
 *     `right_edge`, so the interior span `right_edge - left_edge == width - 5`
 *     leaves `(width - 5) % 8` columns to the tail. Geometries below cover
 *     every residue class 0..7, which is the ADR-1057 bug class (a).
 *   - The mirrored left/right edge columns, which go through `edge_16()`
 *     rather than the vector path — bug class (b).
 *   - Accumulator width: the 5 taps sum to exactly 65536, so a full-range
 *     0xFFFF input drives the u32 accumulator to 0xFFFF0000, one rounding
 *     add short of wrapping. The all-max pattern pins that down — bug class (c).
 *   - Out-of-bounds writes: `dst` carries a 0xA5A5 sentinel border that must
 *     survive every call.
 *
 * Strides are deliberately not equal to the width, and not equal to each
 * other, so a kernel that confused `src_stride` with `dst_stride` (or with
 * `width`) cannot pass.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "test.h"

#if ARCH_AARCH64
#include "common/alignment.h"
#include "feature/arm64/motion_neon.h"
#include "feature/integer_motion.h"

/*
 * Scalar reference, transcribed from the upstream Netflix
 * `integer_motion.c::x_convolution_16` (static there, and in this fork
 * replaced at the call site by the fused `motion_score_pipeline_*`, so it
 * cannot be linked). Kept line-for-line equivalent to the same reference used
 * by `test_motion_avx512_parity.c::x_conv_16_scalar`, so the NEON and AVX-512
 * kernels are held to one shared contract.
 */
static void ref_x_convolution_16(const uint16_t *src, uint16_t *dst, unsigned width,
                                 unsigned height, ptrdiff_t src_stride, ptrdiff_t dst_stride)
{
    const unsigned radius = (unsigned)(filter_width / 2);
    const unsigned left_edge = (unsigned)vmaf_ceiln((int)radius, 1);
    const unsigned right_edge =
        (unsigned)vmaf_floorn((int)(width - (unsigned)(filter_width - (int)radius)), 1);
    const unsigned shift_add_round = 32768u;

    for (unsigned i = 0; i < height; ++i) {
        for (unsigned j = 0; j < left_edge; j++) {
            dst[(ptrdiff_t)i * dst_stride + j] =
                (uint16_t)((edge_16(true, src, (int)width, (int)height, (int)src_stride, (int)i,
                                    (int)j) +
                            shift_add_round) >>
                           16);
        }

        for (unsigned j = left_edge; j < right_edge; j++) {
            uint32_t accum = 0;
            for (int k = 0; k < filter_width; ++k) {
                accum += (uint32_t)filter[k] *
                         (uint32_t)src[(ptrdiff_t)i * src_stride + (ptrdiff_t)j - radius + k];
            }
            dst[(ptrdiff_t)i * dst_stride + j] = (uint16_t)((accum + shift_add_round) >> 16);
        }

        for (unsigned j = right_edge; j < width; j++) {
            dst[(ptrdiff_t)i * dst_stride + j] =
                (uint16_t)((edge_16(true, src, (int)width, (int)height, (int)src_stride, (int)i,
                                    (int)j) +
                            shift_add_round) >>
                           16);
        }
    }
}

enum { PAD = 8, SENTINEL = 0xA5A5 };

/* Deterministic xorshift32 — same generator the other SIMD parity tests use. */
static uint16_t next_u16(uint32_t *state)
{
    uint32_t s = *state;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    *state = s;
    return (uint16_t)(s >> 8);
}

enum pattern { PAT_RANDOM = 0, PAT_MAX = 1, PAT_ALTERNATING = 2, PAT_RAMP = 3 };

static const char *pattern_name(enum pattern p)
{
    switch (p) {
    case PAT_RANDOM:
        return "random";
    case PAT_MAX:
        return "all-0xFFFF";
    case PAT_ALTERNATING:
        return "alternating";
    case PAT_RAMP:
    default:
        return "ramp";
    }
}

/*
 * One (width, height, pattern) case. Returns the number of mismatching
 * elements; reports the first few, plus the column histogram needed to make
 * an arithmetic argument about *which* region diverged.
 */
static int check_case(unsigned w, unsigned h, enum pattern pat, int *sentinel_broken)
{
    const ptrdiff_t src_stride = (ptrdiff_t)w + 7;
    const ptrdiff_t dst_stride = (ptrdiff_t)w + 5;
    const size_t src_n = (size_t)(src_stride)*h + PAD;
    const size_t dst_n = (size_t)(dst_stride) * (h + PAD) + PAD;

    uint16_t *src = malloc(src_n * sizeof(uint16_t));
    uint16_t *dst_ref = malloc(dst_n * sizeof(uint16_t));
    uint16_t *dst_neon = malloc(dst_n * sizeof(uint16_t));
    if (!src || !dst_ref || !dst_neon) {
        free(src);
        free(dst_ref);
        free(dst_neon);
        (void)fprintf(stderr, "  allocation failure at %ux%u\n", w, h);
        return -1;
    }

    uint32_t seed = 0x1234567u ^ (w * 2654435761u) ^ (h * 40503u) ^ ((uint32_t)pat * 97u);
    for (size_t i = 0; i < src_n; ++i) {
        switch (pat) {
        case PAT_MAX:
            src[i] = 0xFFFFu;
            break;
        case PAT_ALTERNATING:
            src[i] = (i & 1u) ? 0xFFFFu : 0u;
            break;
        case PAT_RAMP:
            src[i] = (uint16_t)((i * 4099u) & 0xFFFFu);
            break;
        case PAT_RANDOM:
        default:
            src[i] = next_u16(&seed);
            break;
        }
    }

    for (size_t i = 0; i < dst_n; ++i) {
        dst_ref[i] = SENTINEL;
        dst_neon[i] = SENTINEL;
    }

    ref_x_convolution_16(src, dst_ref, w, h, src_stride, dst_stride);
    x_convolution_16_neon(src, dst_neon, w, h, src_stride, dst_stride);

    int mismatches = 0;
    int interior_mismatches = 0;
    int edge_mismatches = 0;
    unsigned first_bad_col = 0;
    unsigned last_bad_col = 0;
    const unsigned left_edge = 2u;
    const unsigned right_edge = (w >= 3u) ? (w - 3u) : 0u;

    for (unsigned i = 0; i < h; ++i) {
        for (unsigned j = 0; j < w; ++j) {
            const size_t idx = (size_t)i * (size_t)dst_stride + j;
            if (dst_ref[idx] == dst_neon[idx])
                continue;
            if (mismatches == 0)
                first_bad_col = j;
            last_bad_col = j;
            if (j >= left_edge && j < right_edge)
                ++interior_mismatches;
            else
                ++edge_mismatches;
            if (mismatches < 6) {
                (void)fprintf(stderr,
                              "  %ux%u %s: dst[%u][%u] scalar=%u neon=%u"
                              " (left_edge=%u right_edge=%u)\n",
                              w, h, pattern_name(pat), i, j, (unsigned)dst_ref[idx],
                              (unsigned)dst_neon[idx], left_edge, right_edge);
            }
            ++mismatches;
        }
    }

    if (mismatches) {
        (void)fprintf(stderr,
                      "  %ux%u %s: %d mismatches (%d interior, %d edge),"
                      " cols [%u..%u], interior span=%u, span%%8=%u\n",
                      w, h, pattern_name(pat), mismatches, interior_mismatches, edge_mismatches,
                      first_bad_col, last_bad_col,
                      (right_edge > left_edge) ? (right_edge - left_edge) : 0u,
                      (right_edge > left_edge) ? ((right_edge - left_edge) % 8u) : 0u);
    }

    /* Nothing outside the w x h destination rectangle may be touched. */
    for (unsigned i = 0; i < h + PAD; ++i) {
        for (ptrdiff_t j = 0; j < dst_stride; ++j) {
            if (i < h && j < (ptrdiff_t)w)
                continue;
            const size_t idx = (size_t)i * (size_t)dst_stride + (size_t)j;
            if (idx >= dst_n)
                break;
            if (dst_neon[idx] != SENTINEL) {
                (void)fprintf(stderr, "  %ux%u %s: NEON wrote outside dst[%u][%td] (=%u)\n", w, h,
                              pattern_name(pat), i, j, (unsigned)dst_neon[idx]);
                *sentinel_broken = 1;
                i = h + PAD; /* stop after the first report */
                break;
            }
        }
    }

    free(src);
    free(dst_ref);
    free(dst_neon);
    return mismatches;
}
#endif /* ARCH_AARCH64 */

static char *test_x_convolution_16_neon_matches_scalar(void)
{
#if !ARCH_AARCH64
    return NULL; /* NEON kernel is aarch64-only. */
#else
    /*
     * The interior span handed to the vector loop is `width - 5`. The widths
     * below cover every residue `(width - 5) % 8`, so a missing or short
     * scalar tail cannot hide:
     *   w=13,21,29,69 -> residue 0 (vector body only)
     *   w=14..20      -> residues 1..7
     *   w=16,40,64,80 -> realistic frame-ish widths with a 3-column tail
     *   w=5,6,8,9,12  -> no vector iteration at all (needs w >= 13)
     *   w=3,4         -> degenerate: right_edge <= left_edge, all columns
     *                    take the mirrored edge path
     * Heights include odd values and 1 to exercise the row loop and strides.
     */
    static const unsigned widths[] = {3,  4,  5,  6,  8,  9,  12, 13, 14, 15, 16,
                                      17, 18, 19, 20, 21, 29, 40, 64, 69, 80};
    static const unsigned heights[] = {1, 3, 5, 12, 17};

    int total_mismatches = 0;
    int sentinel_broken = 0;

    for (size_t wi = 0; wi < sizeof(widths) / sizeof(widths[0]); ++wi) {
        for (size_t hi = 0; hi < sizeof(heights) / sizeof(heights[0]); ++hi) {
            for (int p = PAT_RANDOM; p <= PAT_RAMP; ++p) {
                const int n =
                    check_case(widths[wi], heights[hi], (enum pattern)p, &sentinel_broken);
                if (n < 0)
                    return "allocation failure";
                total_mismatches += n;
            }
        }
    }

    if (total_mismatches) {
        (void)fprintf(stderr, "x_convolution_16_neon: %d total mismatching elements\n",
                      total_mismatches);
    }
    mu_assert("x_convolution_16_neon wrote outside the destination rectangle", !sentinel_broken);
    mu_assert("x_convolution_16_neon diverges from the scalar reference", total_mismatches == 0);
    return NULL;
#endif
}

char *run_tests(void)
{
#if ARCH_AARCH64
    mu_run_test(test_x_convolution_16_neon_matches_scalar);
#else
    (void)fprintf(stderr, "skipping: non-aarch64 arch\n");
    (void)test_x_convolution_16_neon_matches_scalar;
#endif
    return NULL;
}
