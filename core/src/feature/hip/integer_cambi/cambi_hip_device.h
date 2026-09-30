/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Device-resident CAMBI on HIP (ADR-1378): the layout shared by the host
 *  wrapper (integer_cambi_hip.c) and the kernels (cambi_score.hip), and the
 *  per-work-item arithmetic both of them and the host unit test
 *  (core/test/test_hip_cambi_device_math.c) run.
 *
 *  The header is C and HIP-C++ at once. Every helper marked CAMBI_HD is the
 *  exact code a kernel executes; the unit test compiles the same functions on
 *  the host, replays every kernel work-item by work-item and checks the frame
 *  score against cambi.c, so a device-free build still pins the numerics. The
 *  design is the SYCL twin's (ADR-1357,
 *  core/src/feature/sycl/integer_cambi_sycl.cpp): integer preprocessing, mask,
 *  mode filter and level map; c-values that keep cambi.c's sliding column
 *  histogram (one work-item owns one histogram column of one row chunk) and
 *  c_value_pixel()'s float formula with the table from
 *  vmaf_cambi_reciprocal_lut(); top-K pooling as a radix select over the IEEE
 *  bit patterns and an exact 128-bit fixed-point sum in units of 2^-24. No
 *  helper here uses fp64.
 *
 *  Kernel arguments: every kernel takes (const CambiHipParams *, int scale,
 *  int pass). CambiHipParams lives in device memory, is written once at init
 *  and holds every buffer pointer and every per-scale dimension, so no large
 *  struct is passed by value (core/src/feature/hip/AGENTS.md, ADR-0759).
 */

#ifndef FEATURE_HIP_INTEGER_CAMBI_CAMBI_HIP_DEVICE_H_
#define FEATURE_HIP_INTEGER_CAMBI_CAMBI_HIP_DEVICE_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__HIPCC__)
#define CAMBI_HD __host__ __device__
#else
#define CAMBI_HD
#endif

#ifdef __cplusplus
#define CAMBI_HIP_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define CAMBI_HIP_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C header, also compiled as C. ADR-1138. */

#define CAMBI_HIP_NUM_SCALES 5
/* Level-map value of a pixel that neither feeds nor queries a histogram:
 * masked out, or outside the [v_band_base, v_band_base + levels) band
 * cambi.c::calculate_c_values() keeps. */
#define CAMBI_HIP_Q_INVALID 0xFFFFu
/* Radix select: 11 + 11 + 10 bits of the IEEE pattern. */
#define CAMBI_HIP_RADIX_BINS 2048u
#define CAMBI_HIP_RADIX_PASSES 3
/* c-value -> fixed point: every non-zero c-value is a multiple of 2^-24. */
#define CAMBI_HIP_FIXED_SCALE 16777216.0f
#define CAMBI_HIP_FIXED_SHIFT 24
#define CAMBI_HIP_STATUS_INVALID_INPUT 1u

/* Launch shapes shared by the host launches and the kernels' shared memory. */
#define CAMBI_HIP_TILE 16u
#define CAMBI_HIP_ROWMASK_ROWS 8u
#define CAMBI_HIP_CVALS_BLOCK 64u
#define CAMBI_HIP_POOL_BLOCK 256u
#define CAMBI_HIP_BINS_PER_LANE (CAMBI_HIP_RADIX_BINS / CAMBI_HIP_POOL_BLOCK)
/* Spatial-mask tile: a (tile + 7)^2 square of samples gives the (tile + 6)^2
 * zero-derivative flags whose 7x7 sums cover the tile. */
#define CAMBI_HIP_MASK_HALO 3u
#define CAMBI_HIP_MASK_FLAGS_W (CAMBI_HIP_TILE + 2u * CAMBI_HIP_MASK_HALO)
#define CAMBI_HIP_MASK_PIX_W (CAMBI_HIP_MASK_FLAGS_W + 1u)
#define CAMBI_HIP_MASK_PIXELS (CAMBI_HIP_MASK_PIX_W * CAMBI_HIP_MASK_PIX_W)
#define CAMBI_HIP_MASK_FLAGS (CAMBI_HIP_MASK_FLAGS_W * CAMBI_HIP_MASK_FLAGS_W)
#define CAMBI_HIP_MASK_ROW_SUMS (CAMBI_HIP_TILE * CAMBI_HIP_MASK_FLAGS_W)
#define CAMBI_HIP_MASK_OUTSIDE 0xFFFFFFFFu

/* Per-scale radix-select state. The scan of pass p writes k_next[p], the rank
 * left inside the chosen bucket, and ORs the chosen bin into prefix; pass 0
 * reads the scale's top-K count from CambiHipScale::topk. The four sums hold
 * the low and high 32-bit halves of per-block fixed-point sums, so the exact
 * total is (hi << 32) + lo in 128 bits. */
typedef struct CambiHipSelect {
    uint32_t hist[CAMBI_HIP_RADIX_BINS];
    uint32_t prefix;
    uint32_t k_next[CAMBI_HIP_RADIX_PASSES];
    /* Pass 0 landed in bin 0, which holds only exact zeros (every non-zero
     * c-value is >= 0.5): the threshold is 0 and later passes do nothing. */
    uint32_t resolved;
    uint32_t reserved;
    uint64_t all_lo; /* every c-value, from the c-values kernel */
    uint64_t all_hi;
    uint64_t gt_lo; /* c-values strictly above the threshold */
    uint64_t gt_hi;
} CambiHipSelect;

/* Everything collect() reads back, in one 88-byte device-to-host copy. */
typedef struct CambiHipResults {
    uint64_t sum_lo[CAMBI_HIP_NUM_SCALES];
    uint64_t sum_hi[CAMBI_HIP_NUM_SCALES];
    uint32_t status;
    uint32_t reserved;
} CambiHipResults;

/* The per-frame device state, zeroed by one memset at the start of a frame. */
typedef struct CambiHipFrameState {
    CambiHipSelect select[CAMBI_HIP_NUM_SCALES];
    CambiHipResults results;
} CambiHipFrameState;

/* One scale, fixed at init. The image of scale s is decimated from the image
 * of scale s - 1 (from the preprocessed frame at scale 0 with the high-res
 * speed-up), mode-filtered horizontally into filtered_h and vertically back
 * into image. The spatial mask is never decimated: cambi.c's repeated
 * stride-2 decimation of the mask samples the full-resolution mask at
 * (y << mask_shift, x << mask_shift). */
typedef struct CambiHipScale {
    uint16_t *image;
    uint16_t *filtered_h;
    const uint16_t *decimate_src;
    uint32_t width;
    uint32_t height;
    uint32_t decimate_src_width;
    uint32_t decimate; /* 1 when this scale starts with a 2x decimation */
    uint32_t mask_shift;
    uint32_t words; /* 32-bit words per row of the run / change masks */
    uint32_t chunks;
    uint32_t chunk_rows;
    uint32_t topk;
    uint32_t pool_groups;
} CambiHipScale;

typedef struct CambiHipParams {
    const void *src;   /* distorted luma, src_width x src_height, packed */
    uint16_t *preproc; /* preprocessed frame (proc_width x proc_height) */
    uint16_t *mask;    /* full-resolution spatial mask */
    uint16_t *q;       /* level map of the current scale */
    uint32_t *runs;
    uint32_t *change;
    float *cvals;
    uint16_t *hist;   /* per-chunk column histograms */
    const float *lut; /* vmaf_cambi_reciprocal_lut() */
    const uint16_t *tvi;
    const int32_t *weights;
    const uint32_t *ori_x; /* resize index tables, NULL when same size */
    const uint32_t *ori_y;
    CambiHipFrameState *frame;
    uint32_t src_width;
    uint32_t src_height;
    uint32_t src_bpc;
    uint32_t src_max; /* (1 << bpc) - 1 */
    uint32_t validate;
    uint32_t same_size;
    uint32_t anti_dither;
    uint32_t proc_width;
    uint32_t proc_height;
    uint32_t mask_index;
    uint32_t pad; /* adjusted window / 2 */
    uint32_t levels;
    uint32_t num_diffs;
    uint32_t vlt_luma;
    uint32_t v_band_base;
    uint32_t reserved;
    CambiHipScale scale[CAMBI_HIP_NUM_SCALES];
} CambiHipParams;

/* The host C compiler and hipcc must agree on every offset: pointers and
 * 32-bit fields only, pointers first, even field counts. */
CAMBI_HIP_STATIC_ASSERT(sizeof(CambiHipScale) == 64u, "CambiHipScale layout");
CAMBI_HIP_STATIC_ASSERT(sizeof(CambiHipParams) == 14u * 8u + 16u * 4u + 5u * 64u,
                        "CambiHipParams layout");
CAMBI_HIP_STATIC_ASSERT(sizeof(CambiHipResults) == 88u, "CambiHipResults layout");
CAMBI_HIP_STATIC_ASSERT(offsetof(CambiHipSelect, all_lo) % 8u == 0u, "CambiHipSelect alignment");
CAMBI_HIP_STATIC_ASSERT(offsetof(CambiHipFrameState, results) % 8u == 0u,
                        "CambiHipFrameState alignment");

/* ------------------------------------------------------------------ */
/* Scalar helpers.                                                     */
/* ------------------------------------------------------------------ */

static inline CAMBI_HD uint32_t cambi_hd_float_bits(float value)
{
#if defined(__HIP_DEVICE_COMPILE__)
    return __float_as_uint(value);
#else
    uint32_t bits = 0u;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
#endif
}

static inline CAMBI_HD float cambi_hd_bits_float(uint32_t bits)
{
#if defined(__HIP_DEVICE_COMPILE__)
    return __uint_as_float(bits);
#else
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
#endif
}

/* Index of the lowest set bit of a non-zero word. */
static inline CAMBI_HD uint32_t cambi_hd_ctz(uint32_t bits)
{
#if defined(__HIP_DEVICE_COMPILE__) || defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_ctz(bits);
#else
    uint32_t index = 0u;
    for (int step = 0; step < 31 && ((bits >> index) & 1u) == 0u; ++step)
        ++index;
    return index;
#endif
}

/* One radix-histogram increment: a device atomic, a plain add on the host
 * (the unit test runs the work-items one after another). */
static inline CAMBI_HD void cambi_hd_add_u32(uint32_t *cell, uint32_t value)
{
#if defined(__HIP_DEVICE_COMPILE__)
    atomicAdd(cell, value);
#else
    *cell += value;
#endif
}

static inline CAMBI_HD void cambi_hd_add_u64(uint64_t *cell, uint64_t value)
{
#if defined(__HIP_DEVICE_COMPILE__)
    atomicAdd((unsigned long long *)cell, (unsigned long long)value);
#else
    *cell += value;
#endif
}

/* Add a partial sum (< 2^64) into a lo/hi pair of 32-bit-half accumulators:
 * lo collects the low halves, hi the high halves, each far below 2^64. */
static inline CAMBI_HD void cambi_hd_add_split(uint64_t *lo, uint64_t *hi, uint64_t sum)
{
    if (sum == 0u)
        return;
    cambi_hd_add_u64(lo, sum & 0xFFFFFFFFu);
    cambi_hd_add_u64(hi, sum >> 32u);
}

/* Fixed point in units of 2^-24; exact for 0 and for every value in
 * [0.5, 2^14), the whole c-value range (ADR-1357). */
static inline CAMBI_HD uint64_t cambi_hd_fixed(float value)
{
    return (uint64_t)(value * CAMBI_HIP_FIXED_SCALE);
}

static inline CAMBI_HD uint32_t cambi_hd_radix_shift(int pass)
{
    return pass == 0 ? 21u : (pass == 1 ? 10u : 0u);
}

/* The bits of the pattern the passes before `pass` have fixed. */
static inline CAMBI_HD uint32_t cambi_hd_radix_known_mask(int pass)
{
    return pass == 0 ? 0u : ~0u << cambi_hd_radix_shift(pass - 1);
}

static inline CAMBI_HD uint32_t cambi_hd_radix_bin(uint32_t bits, int pass)
{
    const uint32_t bin_mask = pass == 2 ? 0x3FFu : 0x7FFu;
    return (bits >> cambi_hd_radix_shift(pass)) & bin_mask;
}

/* mode3 is symmetric in its arguments, so cambi.c::filter_mode's cyclic row
 * buffer order does not matter. */
static inline CAMBI_HD uint16_t cambi_hd_mode3(uint16_t first, uint16_t second, uint16_t third)
{
    if (first == second || first == third)
        return first;
    if (second == third)
        return second;
    if (first < second)
        return first < third ? first : third;
    return second < third ? second : third;
}

/* Level-map value of a filtered pixel (see CAMBI_HIP_Q_INVALID). */
static inline CAMBI_HD uint16_t cambi_hd_level(uint16_t value, uint16_t mask, uint32_t v_band_base,
                                               uint32_t levels)
{
    const uint16_t compact = (uint16_t)(value - v_band_base);
    return (mask != 0u && compact < levels) ? compact : (uint16_t)CAMBI_HIP_Q_INVALID;
}

/* ------------------------------------------------------------------ */
/* Preprocessing (cambi.c::cambi_preprocessing).                       */
/* ------------------------------------------------------------------ */

/* cambi.c::validate_image: a sample above (1 << bpc) - 1. */
static inline CAMBI_HD int cambi_hd_sample_invalid(const CambiHipParams *p, uint32_t x, uint32_t y)
{
    const uint32_t off = y * p->src_width + x;
    const uint32_t v = p->src_bpc <= 8u ? (uint32_t)((const uint8_t *)p->src)[off] :
                                          (uint32_t)((const uint16_t *)p->src)[off];
    return v > p->src_max;
}

/* One output sample of decimate_generic_*_and_convert_to_10b. */
static inline CAMBI_HD uint32_t cambi_hd_preproc_sample(const CambiHipParams *p, uint32_t i,
                                                        uint32_t j)
{
    const uint32_t row = p->same_size ? i : p->ori_y[i];
    const uint32_t col = p->same_size ? j : p->ori_x[j];
    const uint32_t off = row * p->src_width + col;
    if (p->src_bpc <= 8u)
        return (uint32_t)((const uint8_t *)p->src)[off] << (10u - p->src_bpc);
    const uint32_t v = ((const uint16_t *)p->src)[off];
    if (p->src_bpc == 9u)
        return v << 1u;
    const uint32_t shift = p->src_bpc - 10u;
    const uint32_t rounding = shift == 0u ? 0u : 1u << (shift - 1u);
    return (v + rounding) >> shift;
}

/* anti_dithering_filter(): the in-place row-major pass only reads samples it
 * has not overwritten yet, so it equals this out-of-place 2x2 average. */
static inline CAMBI_HD uint16_t cambi_hd_preproc_pixel(const CambiHipParams *p, uint32_t i,
                                                       uint32_t j)
{
    const uint32_t here = cambi_hd_preproc_sample(p, i, j);
    if (!p->anti_dither)
        return (uint16_t)here;
    const int last_row = i + 1u == p->proc_height;
    const int last_col = j + 1u == p->proc_width;
    if (last_row && last_col)
        return (uint16_t)here;
    if (last_row)
        return (uint16_t)((here + cambi_hd_preproc_sample(p, i, j + 1u)) >> 1);
    if (last_col)
        return (uint16_t)((here + cambi_hd_preproc_sample(p, i + 1u, j)) >> 1);
    const uint32_t sum = here + cambi_hd_preproc_sample(p, i, j + 1u) +
                         cambi_hd_preproc_sample(p, i + 1u, j) +
                         cambi_hd_preproc_sample(p, i + 1u, j + 1u);
    return (uint16_t)(sum >> 2);
}

/* ------------------------------------------------------------------ */
/* Spatial mask (cambi.c::get_spatial_mask_for_index), one tile of     */
/* CAMBI_HIP_TILE^2 outputs per work-group in four phases.             */
/* ------------------------------------------------------------------ */

/* Phase 1: tile sample i of the tile whose flag origin is (y0, x0) in image
 * coordinates (may be negative), or CAMBI_HIP_MASK_OUTSIDE. */
static inline CAMBI_HD uint32_t cambi_hd_mask_tile_pixel(const CambiHipParams *p, int y0, int x0,
                                                         uint32_t i)
{
    const int y = y0 + (int)(i / CAMBI_HIP_MASK_PIX_W);
    const int x = x0 + (int)(i % CAMBI_HIP_MASK_PIX_W);
    if (y < 0 || x < 0 || (uint32_t)y >= p->proc_height || (uint32_t)x >= p->proc_width)
        return CAMBI_HIP_MASK_OUTSIDE;
    return p->preproc[(uint32_t)y * p->proc_width + (uint32_t)x];
}

/* Phase 2: zero-derivative flag i: equal to the right and the lower
 * neighbour, a missing neighbour (image edge) counting as equal, and 0
 * outside the image exactly as the zero-padded summed-area table counts it. */
static inline CAMBI_HD uint8_t cambi_hd_mask_tile_flag(const uint32_t *pixels, uint32_t i)
{
    const uint32_t r = i / CAMBI_HIP_MASK_FLAGS_W;
    const uint32_t c = i % CAMBI_HIP_MASK_FLAGS_W;
    const uint32_t pixel = pixels[r * CAMBI_HIP_MASK_PIX_W + c];
    if (pixel == CAMBI_HIP_MASK_OUTSIDE)
        return 0u;
    const uint32_t right = pixels[r * CAMBI_HIP_MASK_PIX_W + c + 1u];
    const uint32_t below = pixels[(r + 1u) * CAMBI_HIP_MASK_PIX_W + c];
    return (uint8_t)((right == CAMBI_HIP_MASK_OUTSIDE || right == pixel) &&
                     (below == CAMBI_HIP_MASK_OUTSIDE || below == pixel));
}

/* Phase 3: horizontal 7-tap sum i of the flags. */
static inline CAMBI_HD uint8_t cambi_hd_mask_tile_row_sum(const uint8_t *flags, uint32_t i)
{
    const uint32_t r = i / CAMBI_HIP_TILE;
    const uint32_t c = i % CAMBI_HIP_TILE;
    uint32_t sum = 0u;
    for (uint32_t d = 0u; d <= 2u * CAMBI_HIP_MASK_HALO; ++d)
        sum += flags[r * CAMBI_HIP_MASK_FLAGS_W + c + d];
    return (uint8_t)sum;
}

/* Phase 4: the vertical 7-tap sum of the row sums against mask_index. */
static inline CAMBI_HD uint16_t cambi_hd_mask_tile_out(const uint8_t *row_sums, uint32_t ly,
                                                       uint32_t lx, uint32_t mask_index)
{
    uint32_t sum = 0u;
    for (uint32_t d = 0u; d <= 2u * CAMBI_HIP_MASK_HALO; ++d)
        sum += row_sums[(ly + d) * CAMBI_HIP_TILE + lx];
    return (uint16_t)(sum > mask_index ? 1u : 0u);
}

/* ------------------------------------------------------------------ */
/* Decimation, mode filter and level map (cambi.c::decimate,           */
/* cambi.c::filter_mode).                                              */
/* ------------------------------------------------------------------ */

/* Strict stride-2 subsample; the in-place CPU walk only reads samples it has
 * not overwritten. */
static inline CAMBI_HD void cambi_hd_decimate_pixel(const CambiHipScale *sc, uint32_t x, uint32_t y)
{
    sc->image[y * sc->width + x] = sc->decimate_src[(y * 2u) * sc->decimate_src_width + x * 2u];
}

/* Horizontal pass: edge columns keep their value (mode3(a, a, b) == a). */
static inline CAMBI_HD void cambi_hd_filter_h_pixel(const CambiHipScale *sc, uint32_t x, uint32_t y)
{
    const uint16_t *row = sc->image + y * sc->width;
    const uint32_t left = x > 0u ? x - 1u : 0u;
    const uint32_t right = x + 1u < sc->width ? x + 1u : sc->width - 1u;
    sc->filtered_h[y * sc->width + x] = cambi_hd_mode3(row[left], row[x], row[right]);
}

/* Vertical pass plus level map. filter_mode writes rows 1 .. height-2 only,
 * so the first and last rows keep their pre-filter value. */
static inline CAMBI_HD void cambi_hd_filter_v_pixel(const CambiHipParams *p,
                                                    const CambiHipScale *sc, uint32_t x, uint32_t y)
{
    const uint32_t w = sc->width;
    const uint32_t idx = y * w + x;
    uint16_t value = sc->image[idx];
    if (y > 0u && y + 1u < sc->height) {
        value =
            cambi_hd_mode3(sc->filtered_h[idx - w], sc->filtered_h[idx], sc->filtered_h[idx + w]);
        sc->image[idx] = value;
    }
    const uint32_t mask_off = (y << sc->mask_shift) * p->proc_width + (x << sc->mask_shift);
    p->q[idx] = cambi_hd_level(value, p->mask[mask_off], p->v_band_base, p->levels);
}

/* ------------------------------------------------------------------ */
/* Row masks and c-values (cambi.c::calculate_c_values).               */
/* ------------------------------------------------------------------ */

/* Run bit (a new run of equal levels starts at x > 0) and change bit (the row
 * leaving the window of row y, y - pad - 1, and the row entering it, y + pad,
 * differ at x; an absent row reads as CAMBI_HIP_Q_INVALID) of one pixel. */
static inline CAMBI_HD void cambi_hd_row_mask_bits(const uint16_t *q, uint32_t width,
                                                   uint32_t height, uint32_t pad, uint32_t x,
                                                   uint32_t y, uint32_t *run, uint32_t *change)
{
    const uint16_t *row = q + y * width;
    *run = (x > 0u && row[x] != row[x - 1u]) ? 1u : 0u;
    const uint16_t leaving =
        y > pad ? q[(y - pad - 1u) * width + x] : (uint16_t)CAMBI_HIP_Q_INVALID;
    const uint16_t entering =
        y + pad < height ? q[(y + pad) * width + x] : (uint16_t)CAMBI_HIP_Q_INVALID;
    *change = leaving != entering ? 1u : 0u;
}

/* Everything one c-values work-item reads, resolved once per kernel. */
typedef struct CambiHdCvals {
    const uint16_t *q;
    const uint32_t *runs;
    const uint32_t *change;
    uint16_t *hist;
    float *cvals;
    uint32_t *radix_hist;
    const float *lut;
    const uint16_t *tvi;
    const int32_t *weights;
    uint32_t width;
    uint32_t height;
    uint32_t words;
    uint32_t pad;
    uint32_t chunk_rows;
    uint32_t levels;
    uint32_t num_diffs;
    uint32_t vlt_luma;
    uint32_t v_band_base;
} CambiHdCvals;

static inline CAMBI_HD CambiHdCvals cambi_hd_cvals_args(const CambiHipParams *p, int scale)
{
    const CambiHipScale *sc = &p->scale[scale];
    CambiHdCvals a;
    a.q = p->q;
    a.runs = p->runs;
    a.change = p->change;
    a.hist = p->hist;
    a.cvals = p->cvals;
    a.radix_hist = p->frame->select[scale].hist;
    a.lut = p->lut;
    a.tvi = p->tvi;
    a.weights = p->weights;
    a.width = sc->width;
    a.height = sc->height;
    a.words = sc->words;
    a.pad = p->pad;
    a.chunk_rows = sc->chunk_rows;
    a.levels = p->levels;
    a.num_diffs = p->num_diffs;
    a.vlt_luma = p->vlt_luma;
    a.v_band_base = p->v_band_base;
    return a;
}

/* Word `w` of a mask row restricted to columns [lo, hi]. */
static inline CAMBI_HD uint32_t cambi_hd_mask_word(const uint32_t *mask_row, uint32_t w,
                                                   uint32_t lo, uint32_t hi)
{
    uint32_t bits = mask_row[w];
    if (w == lo >> 5u)
        bits &= ~0u << (lo & 31u);
    if (w == hi >> 5u && (hi & 31u) != 31u)
        bits &= (1u << ((hi & 31u) + 1u)) - 1u;
    return bits;
}

static inline CAMBI_HD int cambi_hd_mask_any(const uint32_t *mask_row, uint32_t lo, uint32_t hi)
{
    uint32_t any = 0u;
    for (uint32_t w = lo >> 5u; w <= hi >> 5u; ++w)
        any |= cambi_hd_mask_word(mask_row, w, lo, hi);
    return any != 0u;
}

/* Apply `count` (+/-) to one cell with uint16 wrap-around, the arithmetic
 * increment_range() / decrement_range() use. Updates commute, so the cell
 * ends at the true window count whatever the order. */
static inline CAMBI_HD void cambi_hd_hist_apply(uint16_t *col_hist, uint32_t width, uint16_t level,
                                                int count)
{
    if (level == CAMBI_HIP_Q_INVALID)
        return;
    uint16_t *cell = &col_hist[(uint32_t)level * width];
    *cell = (uint16_t)((int)*cell + count);
}

/* Add (sign +1) or remove (sign -1) row y's pixels in [lo, hi], one update
 * per run of equal levels. */
static inline CAMBI_HD void cambi_hd_hist_row_runs(const CambiHdCvals *a, uint16_t *col_hist,
                                                   uint32_t y, uint32_t lo, uint32_t hi, int sign)
{
    const uint16_t *row = a->q + y * a->width;
    const uint32_t *runs = a->runs + y * a->words;
    uint32_t start = lo;
    for (uint32_t w = (lo + 1u) >> 5u; lo < hi && w <= hi >> 5u; ++w) {
        uint32_t bits = cambi_hd_mask_word(runs, w, lo + 1u, hi);
        for (int visited = 0; visited < 32 && bits != 0u; ++visited) {
            const uint32_t x = w * 32u + cambi_hd_ctz(bits);
            bits &= bits - 1u;
            cambi_hd_hist_apply(col_hist, a->width, row[start], sign * (int)(x - start));
            start = x;
        }
    }
    cambi_hd_hist_apply(col_hist, a->width, row[start], sign * (int)(hi + 1u - start));
}

/* Move the window of a column from row y - 1 to row y: remove row
 * y - pad - 1, add row y + pad; skipped when both agree over the window. */
static inline CAMBI_HD void cambi_hd_hist_slide(const CambiHdCvals *a, uint16_t *col_hist,
                                                uint32_t y, uint32_t lo, uint32_t hi)
{
    if (!cambi_hd_mask_any(a->change + y * a->words, lo, hi))
        return;
    if (y > a->pad)
        cambi_hd_hist_row_runs(a, col_hist, y - a->pad - 1u, lo, hi, -1);
    if (y + a->pad < a->height)
        cambi_hd_hist_row_runs(a, col_hist, y + a->pad, lo, hi, 1);
}

/* cambi.c::c_value_pixel for the pixel whose level-map value is q0, float for
 * float: one int-to-float conversion and one multiply by the same table. */
static inline CAMBI_HD float cambi_hd_cvals_pixel(const CambiHdCvals *a, const uint16_t *col_hist,
                                                  uint16_t q0)
{
    if (q0 == CAMBI_HIP_Q_INVALID)
        return 0.0f;
    const uint32_t value = (uint32_t)q0 + a->v_band_base + a->num_diffs;
    const int p0 = col_hist[(uint32_t)q0 * a->width];
    float c_value = 0.0f;
    for (uint32_t d = 0u; d < a->num_diffs; ++d) {
        if (value > a->tvi[d] || value + d + 1u <= a->vlt_luma)
            continue;
        const uint32_t up = (uint32_t)q0 + d + 1u;
        const int p1 = up < a->levels ? col_hist[up * a->width] : 0;
        const int p2 = q0 >= d + 1u ? col_hist[((uint32_t)q0 - d - 1u) * a->width] : 0;
        const int pm = p1 > p2 ? p1 : p2;
        const float val = (float)(a->weights[d] * p0 * pm) * a->lut[pm + p0];
        if (val > c_value)
            c_value = val;
    }
    return c_value;
}

/* Zero the column, then load the window of the chunk's first row. */
static inline CAMBI_HD void cambi_hd_cvals_prime(const CambiHdCvals *a, uint16_t *col_hist,
                                                 uint32_t y0, uint32_t lo, uint32_t hi)
{
    for (uint32_t level = 0u; level < a->levels; ++level)
        col_hist[level * a->width] = 0u;
    const uint32_t first = y0 > a->pad ? y0 - a->pad : 0u;
    const uint32_t last = y0 + a->pad < a->height ? y0 + a->pad : a->height - 1u;
    for (uint32_t y = first; y <= last; ++y)
        cambi_hd_hist_row_runs(a, col_hist, y, lo, hi, 1);
}

/* A work-item's share of top-K pass 0: the radix count of its c-values (one
 * atomic per run of equal bins) and their fixed-point sum. */
typedef struct CambiHdTally {
    uint64_t sum;
    uint32_t bin;
    uint32_t count;
} CambiHdTally;

static inline CAMBI_HD void cambi_hd_tally_flush(uint32_t *radix_hist, CambiHdTally *tally)
{
    if (tally->count != 0u) {
        cambi_hd_add_u32(&radix_hist[tally->bin], tally->count);
        tally->count = 0u;
    }
}

static inline CAMBI_HD void cambi_hd_tally_add(uint32_t *radix_hist, CambiHdTally *tally,
                                               float value)
{
    const uint32_t bin = cambi_hd_radix_bin(cambi_hd_float_bits(value), 0);
    if (bin != tally->bin) {
        cambi_hd_tally_flush(radix_hist, tally);
        tally->bin = bin;
    }
    ++tally->count;
    tally->sum += cambi_hd_fixed(value);
}

/* One work-item's column of one row chunk: rows [y0, y1), window columns
 * [lo, hi], and its histogram column. */
typedef struct CambiHdColumn {
    uint16_t *col_hist;
    uint32_t col;
    uint32_t y0;
    uint32_t y1;
    uint32_t lo;
    uint32_t hi;
} CambiHdColumn;

/* Resolve histogram column `col` of row chunk `chunk` and load the window of
 * the chunk's first row; 0 when the work-item has no column. The window of
 * row y covers rows [y - pad, y + pad] and columns [col - pad, col + pad],
 * clipped to the image, as calculate_c_values()'s first-pass / top-edge /
 * middle-slide / bottom-edge walk leaves it. */
static inline CAMBI_HD int cambi_hd_cvals_begin(const CambiHdCvals *a, uint32_t chunk, uint32_t col,
                                                CambiHdColumn *c)
{
    c->y0 = chunk * a->chunk_rows;
    if (col >= a->width || c->y0 >= a->height)
        return 0;
    c->col = col;
    c->y1 = c->y0 + a->chunk_rows < a->height ? c->y0 + a->chunk_rows : a->height;
    c->lo = col > a->pad ? col - a->pad : 0u;
    c->hi = col + a->pad < a->width ? col + a->pad : a->width - 1u;
    c->col_hist = a->hist + chunk * a->levels * a->width + col;
    cambi_hd_cvals_prime(a, c->col_hist, c->y0, c->lo, c->hi);
    return 1;
}

/* Row y of a begun column: slide the window down (after the first row), then
 * the pixel's c-value and its pass-0 tally. */
static inline CAMBI_HD void cambi_hd_cvals_row(const CambiHdCvals *a, const CambiHdColumn *c,
                                               uint32_t y, CambiHdTally *tally)
{
    if (y > c->y0)
        cambi_hd_hist_slide(a, c->col_hist, y, c->lo, c->hi);
    const uint32_t idx = y * a->width + c->col;
    const float value = cambi_hd_cvals_pixel(a, c->col_hist, a->q[idx]);
    a->cvals[idx] = value;
    cambi_hd_tally_add(a->radix_hist, tally, value);
}

/* One work-item: histogram column `col` of row chunk `chunk`, row by row. */
static inline CAMBI_HD void cambi_hd_cvals_column(const CambiHdCvals *a, uint32_t chunk,
                                                  uint32_t col, CambiHdTally *tally)
{
    CambiHdColumn c;
    if (!cambi_hd_cvals_begin(a, chunk, col, &c))
        return;
    for (uint32_t y = c.y0; y < c.y1; ++y)
        cambi_hd_cvals_row(a, &c, y, tally);
}

/* ------------------------------------------------------------------ */
/* Top-K pooling (cambi.c::spatial_pooling).                           */
/* ------------------------------------------------------------------ */

/* The rank pass `pass` looks for inside the bucket the earlier passes chose. */
static inline CAMBI_HD uint32_t cambi_hd_pass_rank(const CambiHipScale *sc,
                                                   const CambiHipSelect *sel, int pass)
{
    return pass == 0 ? sc->topk : sel->k_next[pass - 1];
}

/* Whether a c-value pattern lies in the bucket passes 0 .. pass-1 chose; if
 * so, its bin of pass `pass`. */
static inline CAMBI_HD int cambi_hd_radix_match(uint32_t bits, uint32_t prefix, int pass,
                                                uint32_t *bin)
{
    if ((bits & cambi_hd_radix_known_mask(pass)) != prefix)
        return 0;
    *bin = cambi_hd_radix_bin(bits, pass);
    return 1;
}

/* One lane of a radix scan once `before`, the element count in every bin
 * above the lane's, is known: lane l owns bins [top - bins + 1, top], visited
 * high to low. The lane whose range holds the k-th largest element records
 * the bin, the rank left inside it and, after pass 0, whether it is bin 0. */
static inline CAMBI_HD void cambi_hd_scan_pick(CambiHipSelect *sel, uint32_t top, uint32_t bins,
                                               uint32_t k, uint32_t before, int pass)
{
    uint32_t cum = before;
    for (uint32_t j = 0u; j < bins; ++j) {
        const uint32_t count = sel->hist[top - j];
        if (cum + count >= k) {
            sel->prefix |= (top - j) << cambi_hd_radix_shift(pass);
            sel->k_next[pass] = k - cum;
            sel->resolved = (pass == 0 && top - j == 0u) ? 1u : 0u;
            return;
        }
        cum += count;
    }
}

/* 128-bit accumulator: value = hi * 2^64 + lo. */
typedef struct CambiHdU128 {
    uint64_t lo;
    uint64_t hi;
} CambiHdU128;

/* hi32 * 2^32 + lo32 as a 128-bit value (both halves < 2^64). */
static inline CAMBI_HD CambiHdU128 cambi_hd_u128_from_halves(uint64_t hi32_sum, uint64_t lo32_sum)
{
    const uint64_t shifted = hi32_sum << 32u;
    CambiHdU128 r;
    r.lo = shifted + lo32_sum;
    r.hi = (hi32_sum >> 32u) + (r.lo < shifted ? 1u : 0u);
    return r;
}

static inline CAMBI_HD CambiHdU128 cambi_hd_u128_add(CambiHdU128 x, CambiHdU128 y)
{
    CambiHdU128 r;
    r.lo = x.lo + y.lo;
    r.hi = x.hi + y.hi + (r.lo < x.lo ? 1u : 0u);
    return r;
}

/* The exact top-K sum of one scale: the elements above the threshold (or
 * every element when the threshold resolved to 0) plus k_rem copies of it. */
static inline CAMBI_HD CambiHdU128 cambi_hd_topk_total(const CambiHipSelect *sel)
{
    const int resolved = sel->resolved != 0u;
    const uint64_t lo = resolved ? sel->all_lo : sel->gt_lo;
    const uint64_t hi = resolved ? sel->all_hi : sel->gt_hi;
    const uint64_t t_fixed = resolved ? 0u : cambi_hd_fixed(cambi_hd_bits_float(sel->prefix));
    const uint64_t k_rem = sel->k_next[CAMBI_HIP_RADIX_PASSES - 1];
    const CambiHdU128 ties =
        cambi_hd_u128_from_halves((t_fixed >> 32u) * k_rem, (t_fixed & 0xFFFFFFFFu) * k_rem);
    return cambi_hd_u128_add(cambi_hd_u128_from_halves(hi, lo), ties);
}

/* Contiguous element block [begin, end) of pooling group `group`. */
static inline CAMBI_HD void cambi_hd_pool_block(uint32_t n, uint32_t groups, uint32_t group,
                                                uint32_t *begin, uint32_t *end)
{
    const uint32_t per_group = (n + groups - 1u) / groups;
    *begin = group * per_group;
    *end = *begin + per_group < n ? *begin + per_group : n;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* FEATURE_HIP_INTEGER_CAMBI_CAMBI_HIP_DEVICE_H_ */
