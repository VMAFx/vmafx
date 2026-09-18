/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Bit-exact parity of the CAMBI spatial-mask row kernels against the scalar
 *  reference, on every ISA that has a twin.
 *
 *  Subjects: compute_dp_row_{avx2,avx512,neon} and
 *  compute_mask_row_{avx2,avx512,neon}. The reference is the production scalar
 *  compute_dp_row / compute_mask_row, the same functions the extractor binds
 *  by default, so the test cannot drift from the scalar fallback.
 *
 *  Both kernels are integer-only. The dp row is a modular uint32 prefix sum and
 *  the mask row an unsigned 32-bit compare of a modular box sum, so SIMD and
 *  scalar must agree byte for byte on every input; SIMD_BITEXACT_ASSERT_MEMCMP
 *  is the assertion, with no tolerance (ADR-0138 / ADR-0139).
 *
 *  Three sweeps per ISA:
 *   1. dp row: every width in g_widths (every tail residue of the 8- and
 *      16-lane steps, odd widths, production widths) plus seeded random
 *      widths, pad sizes 1..8, deriv_valid true and false, derivative values
 *      both 0/1 (what the pipeline produces) and full-range uint16, and a
 *      full-range uint32 previous row so the prefix wraps.
 *   2. mask row: the same widths and pads against full-range uint32 dp rows,
 *      with box sums planted at mask_index - 1, mask_index and mask_index + 1
 *      for thresholds that include 0, 2^31 - 1, 2^31 and 2^32 - 1. That pins
 *      the unsigned compare at the signed/unsigned boundary.
 *   3. chain: the whole spatial-mask recurrence for several heights and mask
 *      filter sizes, built row by row with the scalar kernels on one side and
 *      the SIMD kernels on the other, including the padding rows the extractor
 *      feeds with deriv_valid = false. Every dp row and every mask row is
 *      compared.
 *
 *  Buffers are heap-allocated to their exact length, so an out-of-bounds read
 *  or write in a kernel tail is visible to AddressSanitizer, and output buffers
 *  start from the same sentinel so a stray write inside the row is visible to
 *  the comparison as well.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "test.h"
/* clang-format off — test.h has no header guard; must precede harness. */
#include "simd_bitexact_test.h"
/* clang-format on */

#include "feature/cambi.h"

#if ARCH_X86
#include "x86/cpu.h"
#include "feature/x86/cambi_avx2.h"
#if HAVE_AVX512
#include "feature/x86/cambi_avx512.h"
#endif
#endif
#if ARCH_AARCH64
#include "feature/arm64/cambi_neon.h"
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

typedef void (*DpRowFn)(uint32_t *dp_curr, const uint32_t *dp_prev, const uint16_t *deriv,
                        int width, int pad_size, bool deriv_valid);
typedef void (*MaskRowFn)(uint16_t *mask_row, const uint32_t *dp_bottom, const uint32_t *dp_top,
                          int width, int pad_size, uint32_t mask_index);

typedef struct {
    DpRowFn dp_row;
    MaskRowFn mask_row;
} RowKernels;

static const RowKernels g_scalar = {compute_dp_row, compute_mask_row};

/* Every tail residue of the 8-lane (AVX2, NEON) and 16-lane (AVX-512) loops,
 * sizes below one vector, odd widths and the production widths the fixtures
 * and common resolutions reach. */
static const int g_widths[] = {1,  2,  3,   4,   5,   6,   7,   8,   9,   10,  11,   12,  13, 14,
                               15, 16, 17,  23,  24,  25,  31,  32,  33,  37,  47,   63,  64, 65,
                               96, 99, 173, 255, 256, 257, 557, 571, 576, 960, 1280, 1920};
#define NUM_WIDTHS (sizeof(g_widths) / sizeof(g_widths[0]))
#define NUM_RANDOM_WIDTHS 24
#define MAX_RANDOM_WIDTH 700

/* pad_size = filter_size >> 1: filters 3, 5, 7 (production), 9, 15 and 17. */
static const int g_pads[] = {1, 2, 3, 4, 7, 8};
#define NUM_PADS (sizeof(g_pads) / sizeof(g_pads[0]))

static const uint32_t g_mask_indices[] = {0u,          1u,          24u,        0x7FFFFFFFu,
                                          0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu};
#define NUM_MASK_INDICES (sizeof(g_mask_indices) / sizeof(g_mask_indices[0]))

static const int g_chain_heights[] = {1, 2, 3, 7, 8, 17, 40};
#define NUM_CHAIN_HEIGHTS (sizeof(g_chain_heights) / sizeof(g_chain_heights[0]))
static const int g_chain_widths[] = {1, 7, 9, 16, 33, 173, 571};
#define NUM_CHAIN_WIDTHS (sizeof(g_chain_widths) / sizeof(g_chain_widths[0]))

static char g_label[160];

/* ---- helpers --------------------------------------------------------- */

static void fill_u32(uint32_t *buf, size_t n, uint32_t *state)
{
    for (size_t i = 0; i < n; i++) {
        buf[i] = simd_test_xorshift32(state);
    }
}

static void fill_u16(uint16_t *buf, size_t n, uint16_t mask, uint32_t *state)
{
    for (size_t i = 0; i < n; i++) {
        buf[i] = (uint16_t)(simd_test_xorshift32(state) & mask);
    }
}

static size_t dp_len(int width, int pad)
{
    return (size_t)width + (size_t)(2 * pad) + 1u;
}

static int random_width(uint32_t *state)
{
    return 1 + (int)(simd_test_xorshift32(state) % MAX_RANDOM_WIDTH);
}

static char *compare_bytes(const void *scalar_buf, const void *simd_buf, size_t n_bytes)
{
    SIMD_BITEXACT_ASSERT_MEMCMP(scalar_buf, simd_buf, n_bytes, g_label);
    return NULL;
}

/* ---- sweep 1: dp row ------------------------------------------------- */

typedef struct {
    uint32_t *prev;
    uint32_t *out_scalar;
    uint32_t *out_simd;
    uint16_t *deriv;
} DpRowBuffers;

static bool dp_row_buffers_alloc(DpRowBuffers *b, int width, int pad)
{
    const size_t n = dp_len(width, pad);
    b->prev = simd_test_aligned_malloc(n * sizeof(uint32_t), 64);
    b->out_scalar = simd_test_aligned_malloc(n * sizeof(uint32_t), 64);
    b->out_simd = simd_test_aligned_malloc(n * sizeof(uint32_t), 64);
    b->deriv = simd_test_aligned_malloc((size_t)width * sizeof(uint16_t), 64);
    return b->prev && b->out_scalar && b->out_simd && b->deriv;
}

static void dp_row_buffers_free(DpRowBuffers *b)
{
    simd_test_aligned_free(b->prev);
    simd_test_aligned_free(b->out_scalar);
    simd_test_aligned_free(b->out_simd);
    simd_test_aligned_free(b->deriv);
}

static char *check_dp_row(const RowKernels *simd, int width, int pad, bool valid,
                          uint16_t deriv_mask, uint32_t seed)
{
    DpRowBuffers b;
    const bool ok = dp_row_buffers_alloc(&b, width, pad);
    char *err = NULL;
    if (ok) {
        const size_t n = dp_len(width, pad);
        uint32_t state = seed;
        fill_u32(b.prev, n, &state);
        fill_u16(b.deriv, (size_t)width, deriv_mask, &state);
        fill_u32(b.out_scalar, n, &state);
        memcpy(b.out_simd, b.out_scalar, n * sizeof(uint32_t));

        g_scalar.dp_row(b.out_scalar, b.prev, b.deriv, width, pad, valid);
        simd->dp_row(b.out_simd, b.prev, b.deriv, width, pad, valid);
        (void)snprintf(g_label, sizeof(g_label), "dp row w=%d pad=%d valid=%d deriv_mask=0x%x",
                       width, pad, (int)valid, (unsigned)deriv_mask);
        err = compare_bytes(b.out_scalar, b.out_simd, n * sizeof(uint32_t));
    }
    dp_row_buffers_free(&b);
    mu_assert("dp row buffer allocation failed", ok);
    return err;
}

static char *check_dp_row_variants(const RowKernels *simd, int width, int pad, uint32_t seed)
{
    static const uint16_t deriv_masks[] = {0x1u, 0xFFFFu};
    for (size_t m = 0; m < 2; m++) {
        for (int v = 0; v < 2; v++) {
            char *err = check_dp_row(simd, width, pad, v == 0, deriv_masks[m],
                                     seed + (uint32_t)(m * 2u + (size_t)v));
            if (err)
                return err;
        }
    }
    return NULL;
}

static char *sweep_dp_rows(const RowKernels *simd)
{
    uint32_t state = 0xC0FFEE11u;
    for (size_t w = 0; w < NUM_WIDTHS + NUM_RANDOM_WIDTHS; w++) {
        const int width = w < NUM_WIDTHS ? g_widths[w] : random_width(&state);
        for (size_t p = 0; p < NUM_PADS; p++) {
            char *err = check_dp_row_variants(simd, width, g_pads[p], simd_test_xorshift32(&state));
            if (err)
                return err;
        }
    }
    return NULL;
}

/* ---- sweep 2: mask row ----------------------------------------------- */

typedef struct {
    uint32_t *bottom;
    uint32_t *top;
    uint16_t *out_scalar;
    uint16_t *out_simd;
} MaskRowBuffers;

static bool mask_row_buffers_alloc(MaskRowBuffers *b, int width, int pad)
{
    const size_t n = dp_len(width, pad);
    b->bottom = simd_test_aligned_malloc(n * sizeof(uint32_t), 64);
    b->top = simd_test_aligned_malloc(n * sizeof(uint32_t), 64);
    b->out_scalar = simd_test_aligned_malloc((size_t)width * sizeof(uint16_t), 64);
    b->out_simd = simd_test_aligned_malloc((size_t)width * sizeof(uint16_t), 64);
    return b->bottom && b->top && b->out_scalar && b->out_simd;
}

static void mask_row_buffers_free(MaskRowBuffers *b)
{
    simd_test_aligned_free(b->bottom);
    simd_test_aligned_free(b->top);
    simd_test_aligned_free(b->out_scalar);
    simd_test_aligned_free(b->out_simd);
}

/* Rewrites dp_top[j + delta] so the box sum at column j lands on
 * mask_index - 1, mask_index, mask_index + 1 or a random value in turn (all
 * modulo 2^32). Columns are planted in ascending order, so dp_top[j] is final
 * before column j reads it. */
static void plant_box_sums(const uint32_t *bottom, uint32_t *top, int width, int pad,
                           uint32_t mask_index, uint32_t *state)
{
    const int delta = 2 * pad + 1;
    for (int j = 0; j < width; j++) {
        const uint32_t pick = simd_test_xorshift32(state);
        const uint32_t target = (pick & 3u) == 3u ? pick : mask_index + (pick & 3u) - 1u;
        top[j + delta] = bottom[j + delta] + top[j] - bottom[j] - target;
    }
}

static char *check_mask_row(const RowKernels *simd, int width, int pad, uint32_t mask_index,
                            uint32_t seed)
{
    MaskRowBuffers b;
    const bool ok = mask_row_buffers_alloc(&b, width, pad);
    char *err = NULL;
    if (ok) {
        const size_t n = dp_len(width, pad);
        uint32_t state = seed;
        fill_u32(b.bottom, n, &state);
        fill_u32(b.top, n, &state);
        plant_box_sums(b.bottom, b.top, width, pad, mask_index, &state);
        fill_u16(b.out_scalar, (size_t)width, 0xFFFFu, &state);
        memcpy(b.out_simd, b.out_scalar, (size_t)width * sizeof(uint16_t));

        g_scalar.mask_row(b.out_scalar, b.bottom, b.top, width, pad, mask_index);
        simd->mask_row(b.out_simd, b.bottom, b.top, width, pad, mask_index);
        (void)snprintf(g_label, sizeof(g_label), "mask row w=%d pad=%d mask_index=0x%x", width, pad,
                       (unsigned)mask_index);
        err = compare_bytes(b.out_scalar, b.out_simd, (size_t)width * sizeof(uint16_t));
    }
    mask_row_buffers_free(&b);
    mu_assert("mask row buffer allocation failed", ok);
    return err;
}

static char *sweep_mask_rows(const RowKernels *simd)
{
    uint32_t state = 0x5EED0B0Bu;
    for (size_t w = 0; w < NUM_WIDTHS + NUM_RANDOM_WIDTHS; w++) {
        const int width = w < NUM_WIDTHS ? g_widths[w] : random_width(&state);
        for (size_t p = 0; p < NUM_PADS; p++) {
            for (size_t m = 0; m < NUM_MASK_INDICES; m++) {
                char *err = check_mask_row(simd, width, g_pads[p], g_mask_indices[m],
                                           simd_test_xorshift32(&state));
                if (err)
                    return err;
            }
        }
    }
    return NULL;
}

/* ---- sweep 3: whole spatial-mask recurrence --------------------------- */

/*
 * Mirrors get_spatial_mask_for_index() in cambi.c on a full, non-cyclic dp
 * matrix: rows 0 .. pad are zero, row pad + 1 + i accumulates derivative row i
 * (deriv_valid only while i < height), and mask row i is the box sum between dp
 * rows i + 2 * pad + 1 and i.
 */
typedef struct {
    int width;
    int height;
    int pad;
    uint32_t mask_index;
    const uint16_t *derivs; /* height rows of width */
} ChainSpec;

static void run_chain(const RowKernels *k, const ChainSpec *c, uint32_t *dp, uint16_t *mask)
{
    const size_t w = dp_len(c->width, c->pad);
    const int dp_rows = c->height + 2 * c->pad + 1;
    memset(dp, 0, (size_t)dp_rows * w * sizeof(uint32_t));
    for (int i = 0; i < c->height + c->pad; i++) {
        const bool valid = i < c->height;
        const uint16_t *deriv = &c->derivs[(size_t)(valid ? i : 0) * (size_t)c->width];
        const size_t row = (size_t)c->pad + 1u + (size_t)i;
        k->dp_row(&dp[row * w], &dp[(row - 1u) * w], deriv, c->width, c->pad, valid);
    }
    for (int i = 0; i < c->height; i++) {
        k->mask_row(&mask[(size_t)i * (size_t)c->width], &dp[(size_t)(i + 2 * c->pad + 1) * w],
                    &dp[(size_t)i * w], c->width, c->pad, c->mask_index);
    }
}

typedef struct {
    uint16_t *derivs;
    uint32_t *dp_scalar;
    uint32_t *dp_simd;
    uint16_t *mask_scalar;
    uint16_t *mask_simd;
} ChainBuffers;

static bool chain_buffers_alloc(ChainBuffers *b, const ChainSpec *c)
{
    const size_t pixels = (size_t)c->width * (size_t)c->height;
    const size_t dp_n = dp_len(c->width, c->pad) * (size_t)(c->height + 2 * c->pad + 1);
    b->derivs = simd_test_aligned_malloc(pixels * sizeof(uint16_t), 64);
    b->dp_scalar = simd_test_aligned_malloc(dp_n * sizeof(uint32_t), 64);
    b->dp_simd = simd_test_aligned_malloc(dp_n * sizeof(uint32_t), 64);
    b->mask_scalar = simd_test_aligned_malloc(pixels * sizeof(uint16_t), 64);
    b->mask_simd = simd_test_aligned_malloc(pixels * sizeof(uint16_t), 64);
    return b->derivs && b->dp_scalar && b->dp_simd && b->mask_scalar && b->mask_simd;
}

static void chain_buffers_free(ChainBuffers *b)
{
    simd_test_aligned_free(b->derivs);
    simd_test_aligned_free(b->dp_scalar);
    simd_test_aligned_free(b->dp_simd);
    simd_test_aligned_free(b->mask_scalar);
    simd_test_aligned_free(b->mask_simd);
}

static char *check_chain(const RowKernels *simd, ChainSpec *c, uint32_t seed)
{
    ChainBuffers b;
    const bool ok = chain_buffers_alloc(&b, c);
    char *err = NULL;
    if (ok) {
        const size_t pixels = (size_t)c->width * (size_t)c->height;
        const size_t dp_n = dp_len(c->width, c->pad) * (size_t)(c->height + 2 * c->pad + 1);
        uint32_t state = seed;
        /* Binary derivatives, as get_derivative_data_for_row produces. */
        fill_u16(b.derivs, pixels, 0x1u, &state);
        c->derivs = b.derivs;
        run_chain(&g_scalar, c, b.dp_scalar, b.mask_scalar);
        run_chain(simd, c, b.dp_simd, b.mask_simd);
        (void)snprintf(g_label, sizeof(g_label), "chain dp w=%d h=%d filter=%d", c->width,
                       c->height, 2 * c->pad + 1);
        err = compare_bytes(b.dp_scalar, b.dp_simd, dp_n * sizeof(uint32_t));
        if (!err) {
            (void)snprintf(g_label, sizeof(g_label), "chain mask w=%d h=%d filter=%d mi=%u",
                           c->width, c->height, 2 * c->pad + 1, (unsigned)c->mask_index);
            err = compare_bytes(b.mask_scalar, b.mask_simd, pixels * sizeof(uint16_t));
        }
    }
    chain_buffers_free(&b);
    mu_assert("chain buffer allocation failed", ok);
    return err;
}

static char *sweep_chains(const RowKernels *simd)
{
    uint32_t state = 0xFACADE01u;
    for (size_t h = 0; h < NUM_CHAIN_HEIGHTS; h++) {
        for (size_t w = 0; w < NUM_CHAIN_WIDTHS; w++) {
            for (size_t p = 0; p < NUM_PADS; p++) {
                const int filter = 2 * g_pads[p] + 1;
                ChainSpec c = {
                    .width = g_chain_widths[w],
                    .height = g_chain_heights[h],
                    .pad = g_pads[p],
                    /* Mid-range threshold: about half the window is flat. */
                    .mask_index = (uint32_t)(filter * filter / 2),
                    .derivs = NULL,
                };
                char *err = check_chain(simd, &c, simd_test_xorshift32(&state));
                if (err)
                    return err;
            }
        }
    }
    return NULL;
}

/* ---- per-ISA entry points -------------------------------------------- */

#if ARCH_X86
static const RowKernels g_avx2 = {compute_dp_row_avx2, compute_mask_row_avx2};

static char *test_avx2_dp_row(void)
{
    return sweep_dp_rows(&g_avx2);
}

static char *test_avx2_mask_row(void)
{
    return sweep_mask_rows(&g_avx2);
}

static char *test_avx2_chain(void)
{
    return sweep_chains(&g_avx2);
}

static char *run_avx2_tests(void)
{
    if (!simd_test_have_avx2())
        return NULL;
    mu_run_test(test_avx2_dp_row);
    mu_run_test(test_avx2_mask_row);
    mu_run_test(test_avx2_chain);
    return NULL;
}

#if HAVE_AVX512
static const RowKernels g_avx512 = {compute_dp_row_avx512, compute_mask_row_avx512};

static char *test_avx512_dp_row(void)
{
    return sweep_dp_rows(&g_avx512);
}

static char *test_avx512_mask_row(void)
{
    return sweep_mask_rows(&g_avx512);
}

static char *test_avx512_chain(void)
{
    return sweep_chains(&g_avx512);
}

static char *run_avx512_tests(void)
{
    if (!simd_test_have_avx512())
        return NULL;
    mu_run_test(test_avx512_dp_row);
    mu_run_test(test_avx512_mask_row);
    mu_run_test(test_avx512_chain);
    return NULL;
}
#endif /* HAVE_AVX512 */
#endif /* ARCH_X86 */

#if ARCH_AARCH64
static const RowKernels g_neon = {compute_dp_row_neon, compute_mask_row_neon};

static char *test_neon_dp_row(void)
{
    return sweep_dp_rows(&g_neon);
}

static char *test_neon_mask_row(void)
{
    return sweep_mask_rows(&g_neon);
}

static char *test_neon_chain(void)
{
    return sweep_chains(&g_neon);
}
#endif /* ARCH_AARCH64 */

char *run_tests(void)
{
#if ARCH_X86
    char *err = run_avx2_tests();
    if (err)
        return err;
#if HAVE_AVX512
    err = run_avx512_tests();
    if (err)
        return err;
#else
    (void)fprintf(stderr, "skipping AVX-512 tests: built without AVX-512\n");
#endif
#elif ARCH_AARCH64
    mu_run_test(test_neon_dp_row);
    mu_run_test(test_neon_mask_row);
    mu_run_test(test_neon_chain);
#else
    (void)fprintf(stderr, "skipping: arch lacks CAMBI spatial-mask SIMD\n");
#endif
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
