/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * NEON-vs-scalar bit-exactness for the integer PSNR per-line SSE kernels.
 *
 * `psnr_sse_line_8_neon` / `psnr_sse_line_16_neon` (core/src/feature/arm64/
 * psnr_neon.c) replace `sse_line_8_c` / `sse_line_16_c` in integer_psnr.c
 * whenever VMAF_ARM_CPU_FLAG_NEON is set. The dispatch site applies **no**
 * width predicate — every plane width, including 1, is routed to NEON — so
 * the kernels own their own scalar tails, and any stride/tail mismatch would
 * land directly in psnr_y / psnr_cb / psnr_cr.
 *
 * What this test pins:
 *   - Bit-exact equality with the transcribed scalar reference over every
 *     width in [1, 40] plus the vector-stride boundaries (32-byte and 8-lane
 *     strides) and realistic plane widths up to 8K.
 *   - Unaligned `ref` / `dis` start pointers, offset independently of each
 *     other (the two planes carry no common alignment guarantee).
 *   - Adversarial fills: max positive diff, max negative diff, zero diff and
 *     an alternating extreme, on top of a reproducible xorshift fill.
 *   - Accumulator width at the extremes. `sse_line_8_c` returns `uint32_t`,
 *     so a line wide enough to exceed 2^32 wraps *in the scalar too*; the
 *     NEON reduction must wrap in the same modulus rather than widen. The
 *     16-bit kernel accumulates into `uint64_t` and must NOT truncate at
 *     2^32 — an 8K line of maximum 16-bit error is 3.3e13.
 */

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

#if ARCH_AARCH64
#include "feature/arm64/psnr_neon.h"

/* Transcribed from sse_line_8_c() in integer_psnr.c (static there).
 * The uint32_t accumulator — and therefore its wraparound — is part of the
 * contract the NEON kernel has to reproduce. */
static uint32_t ref_sse_line_8(const uint8_t *ref, const uint8_t *dis, unsigned w)
{
    uint32_t sse = 0;
    for (unsigned j = 0; j < w; j++) {
        const int16_t e = (int16_t)(ref[j] - dis[j]);
        sse += (uint32_t)(e * e);
    }
    return sse;
}

/* Transcribed from sse_line_16_c() in integer_psnr.c (static there). */
static uint64_t ref_sse_line_16(const uint16_t *ref, const uint16_t *dis, unsigned w)
{
    uint64_t sse = 0;
    for (unsigned j = 0; j < w; j++) {
        const uint32_t e = (uint32_t)abs((int32_t)ref[j] - (int32_t)dis[j]);
        sse += (uint64_t)e * e;
    }
    return sse;
}

/* Widths: every value up to 40 (covers w < stride, the 32/16/8 boundaries and
 * their +/-1 neighbours), the higher stride multiples, and real plane widths
 * (SD/HD/4K/8K luma plus their 4:2:0 chroma halves) with +/-1 neighbours.
 * Kept ascending — the last element is used as the allocation size. */
static const unsigned k_widths[] = {
    1,   2,   3,   4,   5,    6,    7,    8,    9,    10,   11,   12,   13,   14,   15,  16,
    17,  18,  19,  20,  21,   22,   23,   24,   25,   26,   27,   28,   29,   30,   31,  32,
    33,  34,  35,  36,  37,   38,   39,   40,   47,   48,   49,   63,   64,   65,   95,  96,
    97,  127, 128, 129, 159,  160,  161,  255,  256,  257,  288,  352,  360,  480,  576, 639,
    640, 641, 720, 960, 1919, 1920, 1921, 2048, 3839, 3840, 3841, 4096, 7679, 7680, 7681};
#define N_WIDTHS (sizeof(k_widths) / sizeof(k_widths[0]))

/* Element offsets applied independently to ref / dis so the two operands start
 * at different alignments — vld1q_u8 / vld1q_u16 are unaligned loads and the
 * picture planes carry no cross-plane alignment guarantee. */
static const unsigned k_off_ref[] = {0, 1, 0, 3, 7};
static const unsigned k_off_dis[] = {0, 0, 1, 5, 2};
#define N_OFFSETS (sizeof(k_off_ref) / sizeof(k_off_ref[0]))

#define PAT_RANDOM 0
#define PAT_MAX_POS 1
#define PAT_MAX_NEG 2
#define PAT_EQUAL 3
#define PAT_ALTERNATING 4
#define N_PATTERNS 5

static const char *pattern_name(int pat)
{
    static const char *const names[N_PATTERNS] = {"random", "max_pos", "max_neg", "equal",
                                                  "alternating"};
    return names[pat];
}

static void fill_pair_8(uint8_t *ref, uint8_t *dis, unsigned w, int pat, uint32_t seed)
{
    uint32_t state = seed;
    for (unsigned j = 0; j < w; j++) {
        const uint32_t r = simd_test_xorshift32(&state);
        switch (pat) {
        case PAT_MAX_POS:
            ref[j] = 255u;
            dis[j] = 0u;
            break;
        case PAT_MAX_NEG:
            ref[j] = 0u;
            dis[j] = 255u;
            break;
        case PAT_EQUAL:
            ref[j] = (uint8_t)(r & 0xFFu);
            dis[j] = ref[j];
            break;
        case PAT_ALTERNATING:
            ref[j] = (j & 1u) ? 255u : 0u;
            dis[j] = (j & 1u) ? 0u : 255u;
            break;
        default:
            ref[j] = (uint8_t)(r & 0xFFu);
            dis[j] = (uint8_t)((r >> 16) & 0xFFu);
            break;
        }
    }
}

static void fill_pair_16(uint16_t *ref, uint16_t *dis, unsigned w, int pat, uint32_t seed)
{
    uint32_t state = seed;
    for (unsigned j = 0; j < w; j++) {
        const uint32_t r = simd_test_xorshift32(&state);
        switch (pat) {
        case PAT_MAX_POS:
            ref[j] = 65535u;
            dis[j] = 0u;
            break;
        case PAT_MAX_NEG:
            ref[j] = 0u;
            dis[j] = 65535u;
            break;
        case PAT_EQUAL:
            ref[j] = (uint16_t)(r & 0xFFFFu);
            dis[j] = ref[j];
            break;
        case PAT_ALTERNATING:
            ref[j] = (j & 1u) ? 65535u : 0u;
            dis[j] = (j & 1u) ? 0u : 65535u;
            break;
        default:
            /* Full 16-bit range on even lanes, 10-bit range on odd lanes, so
             * one sweep covers both hbd regimes the extractor sees. */
            ref[j] = (j & 1u) ? (uint16_t)(r & 0x3FFu) : (uint16_t)(r & 0xFFFFu);
            dis[j] = (j & 1u) ? (uint16_t)((r >> 11) & 0x3FFu) : (uint16_t)((r >> 16) & 0xFFFFu);
            break;
        }
    }
}
#endif /* ARCH_AARCH64 */

static char *test_psnr_sse_line_8_neon_matches_scalar(void)
{
#if !ARCH_AARCH64
    return NULL; /* NEON kernel is aarch64-only. */
#else
    const unsigned max_w = k_widths[N_WIDTHS - 1];
    const size_t bytes = (size_t)max_w + 16u;
    uint8_t *ref_buf = malloc(bytes);
    uint8_t *dis_buf = malloc(bytes);
    mu_assert("allocation failed", ref_buf != NULL && dis_buf != NULL);

    for (size_t iw = 0; iw < N_WIDTHS; ++iw) {
        const unsigned w = k_widths[iw];
        for (int pat = 0; pat < N_PATTERNS; ++pat) {
            for (size_t io = 0; io < N_OFFSETS; ++io) {
                uint8_t *ref = ref_buf + k_off_ref[io];
                uint8_t *dis = dis_buf + k_off_dis[io];
                const uint32_t seed =
                    0x5eed0000u ^ (uint32_t)(w * 131u + (unsigned)pat * 7u + (unsigned)io);

                /* Poison the whole buffer so any over-read past `w` shows up. */
                memset(ref_buf, 0xA5, bytes);
                memset(dis_buf, 0x5A, bytes);
                fill_pair_8(ref, dis, w, pat, seed);

                const uint32_t expected = ref_sse_line_8(ref, dis, w);
                const uint32_t got = psnr_sse_line_8_neon(ref, dis, w);
                if (expected != got) {
                    (void)fprintf(stderr,
                                  "  w=%u pat=%s off=(%u,%u): scalar %u != neon %u "
                                  "(delta %lld)\n",
                                  w, pattern_name(pat), k_off_ref[io], k_off_dis[io], expected, got,
                                  (long long)got - (long long)expected);
                    free(ref_buf);
                    free(dis_buf);
                    mu_assert("psnr_sse_line_8_neon diverges from the scalar reference", 0);
                }
            }
        }
    }

    free(ref_buf);
    free(dis_buf);
    return NULL;
#endif
}

static char *test_psnr_sse_line_16_neon_matches_scalar(void)
{
#if !ARCH_AARCH64
    return NULL;
#else
    const unsigned max_w = k_widths[N_WIDTHS - 1];
    const size_t bytes = ((size_t)max_w + 16u) * sizeof(uint16_t);
    uint16_t *ref_buf = malloc(bytes);
    uint16_t *dis_buf = malloc(bytes);
    mu_assert("allocation failed", ref_buf != NULL && dis_buf != NULL);

    for (size_t iw = 0; iw < N_WIDTHS; ++iw) {
        const unsigned w = k_widths[iw];
        for (int pat = 0; pat < N_PATTERNS; ++pat) {
            for (size_t io = 0; io < N_OFFSETS; ++io) {
                uint16_t *ref = ref_buf + k_off_ref[io];
                uint16_t *dis = dis_buf + k_off_dis[io];
                const uint32_t seed =
                    0xbeef0000u ^ (uint32_t)(w * 131u + (unsigned)pat * 7u + (unsigned)io);

                memset(ref_buf, 0xA5, bytes);
                memset(dis_buf, 0x5A, bytes);
                fill_pair_16(ref, dis, w, pat, seed);

                const uint64_t expected = ref_sse_line_16(ref, dis, w);
                const uint64_t got = psnr_sse_line_16_neon(ref, dis, w);
                if (expected != got) {
                    (void)fprintf(stderr, "  w=%u pat=%s off=(%u,%u): scalar %llu != neon %llu\n",
                                  w, pattern_name(pat), k_off_ref[io], k_off_dis[io],
                                  (unsigned long long)expected, (unsigned long long)got);
                    free(ref_buf);
                    free(dis_buf);
                    mu_assert("psnr_sse_line_16_neon diverges from the scalar reference", 0);
                }
            }
        }
    }

    free(ref_buf);
    free(dis_buf);
    return NULL;
#endif
}

#if ARCH_AARCH64
/* 8-bit half of the accumulator-extremes test.
 *
 * The per-line SSE of a maximum-error line is w * 255^2 = w * 65025. That
 * exceeds UINT32_MAX at w >= 66052, and `sse_line_8_c` — the upstream scalar —
 * wraps there because it returns `uint32_t`. The NEON kernel must reproduce
 * that wrap exactly: its uint32x4 lanes stay congruent mod 2^32 to the exact
 * partial sums, so the ADDV reduction is congruent too. A "fix" that widened
 * only the NEON reduction to 64 bits would silently diverge from the scalar.
 * Real plane widths never reach the wrap: 8K luma (7680) tops out at 4.99e8. */
static char *check_sse_line_8_wrap(void)
{
    /* 7680:  8K luma, no wrap (499392000).
     * 66051: last width that still fits in uint32 (4294966275).
     * 66052: first width that wraps (4295031300 -> 64004).
     * 70000: wraps by more than one 65025 step (4551750000 -> 256782704). */
    static const unsigned wrap_widths[] = {7680u, 66051u, 66052u, 70000u};
    const unsigned max_w = 70000u;

    uint8_t *ref8 = malloc(max_w);
    uint8_t *dis8 = malloc(max_w);
    mu_assert("allocation failed", ref8 != NULL && dis8 != NULL);
    memset(ref8, 255, max_w);
    memset(dis8, 0, max_w);

    for (size_t i = 0; i < sizeof(wrap_widths) / sizeof(wrap_widths[0]); ++i) {
        const unsigned w = wrap_widths[i];
        const uint64_t exact = (uint64_t)w * 65025u;
        const uint32_t expected = (uint32_t)exact;
        const uint32_t scalar = ref_sse_line_8(ref8, dis8, w);
        const uint32_t neon = psnr_sse_line_8_neon(ref8, dis8, w);
        if (scalar != expected || neon != expected) {
            (void)fprintf(stderr, "  w=%u: exact %llu, expected(mod 2^32) %u, scalar %u, neon %u\n",
                          w, (unsigned long long)exact, expected, scalar, neon);
            free(ref8);
            free(dis8);
            mu_assert("psnr_sse_line_8_neon accumulator wrap differs from the scalar", 0);
        }
    }
    free(ref8);
    free(dis8);
    return NULL;
}

/* 16-bit half: 8K luma of maximum 16-bit error is 7680 * 65535^2 = 3.298e13,
 * 7680x above UINT32_MAX. Any 32-bit truncation inside the NEON kernel — or a
 * saturating widening intrinsic — shows up here. */
static char *check_sse_line_16_no_truncation(void)
{
    const unsigned w16 = 7680u;
    uint16_t *ref16 = malloc((size_t)w16 * sizeof(uint16_t));
    uint16_t *dis16 = malloc((size_t)w16 * sizeof(uint16_t));
    mu_assert("allocation failed", ref16 != NULL && dis16 != NULL);
    for (unsigned j = 0; j < w16; ++j) {
        ref16[j] = 65535u;
        dis16[j] = 0u;
    }

    const uint64_t exact16 = (uint64_t)w16 * 65535ull * 65535ull;
    const uint64_t scalar16 = ref_sse_line_16(ref16, dis16, w16);
    const uint64_t neon16 = psnr_sse_line_16_neon(ref16, dis16, w16);
    const int ok16 = (exact16 == 32984342208000ull) && (scalar16 == exact16) && (neon16 == exact16);
    if (!ok16) {
        (void)fprintf(stderr, "  w=%u: exact %llu, scalar %llu, neon %llu\n", w16,
                      (unsigned long long)exact16, (unsigned long long)scalar16,
                      (unsigned long long)neon16);
    }
    free(ref16);
    free(dis16);
    mu_assert("psnr_sse_line_16_neon truncates its 64-bit accumulator", ok16);
    return NULL;
}
#endif /* ARCH_AARCH64 */

static char *test_psnr_sse_line_neon_accumulator_extremes(void)
{
#if !ARCH_AARCH64
    return NULL;
#else
    char *msg = check_sse_line_8_wrap();
    if (msg != NULL) {
        return msg;
    }
    return check_sse_line_16_no_truncation();
#endif
}

char *run_tests(void)
{
#if ARCH_AARCH64
    mu_run_test(test_psnr_sse_line_8_neon_matches_scalar);
    mu_run_test(test_psnr_sse_line_16_neon_matches_scalar);
    mu_run_test(test_psnr_sse_line_neon_accumulator_extremes);
#else
    (void)fprintf(stderr, "skipping: non-aarch64 arch\n");
    (void)test_psnr_sse_line_8_neon_matches_scalar;
    (void)test_psnr_sse_line_16_neon_matches_scalar;
    (void)test_psnr_sse_line_neon_accumulator_extremes;
#endif
    return NULL;
}
