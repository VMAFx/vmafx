/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Frame-level CAMBI c-values walk shared by the fork's AVX2, AVX-512 and
 *  NEON drivers (calculate_c_values_scan_avx2 / calculate_c_values_avx512 /
 *  calculate_c_values_neon).
 *
 *  It is the walk calculate_c_values() in cambi.c and calculate_c_values_avx2()
 *  in x86/cambi_avx2.c spell out: a first pass that fills the sliding column
 *  histogram with the top pad_size rows, then top edge, middle slide and bottom
 *  edge, each updating the histogram and computing one c-values row. The
 *  histogram updates are the same cambi.h helpers the scalar walk calls
 *  (update_histogram_*_edge, uh_slide_edge), with the same range updaters.
 *
 *  What differs is how the columns are visited. The scalar walk calls a helper
 *  for every column, and on flat, banding-prone content almost every call
 *  returns without touching the histogram: the pixel is masked out, out of the
 *  scored band, or (in the middle slide) equal to the pixel it replaces. Here a
 *  per-ISA scan tests a block of up to CAMBI_SCAN_BLOCK columns with vector
 *  compares and returns a bit per column that may need an update; the helper
 *  runs only for those. A scan may flag more columns than needed (the helper
 *  re-checks and returns), but must never miss one.
 *
 *  Bit-exactness: every column that updates the histogram in the scalar walk
 *  runs the same helper with the same range here, and the updates within a row
 *  commute (modular uint16 increments and decrements, all applied before the
 *  row's c-values are computed), so the histogram each c-values row sees is the
 *  scalar one. With a row kernel that is bit-exact against
 *  calculate_c_values_row(), the c-values equal the scalar calculate_c_values()
 *  output byte for byte. The middle columns use the clamped (edge) helpers:
 *  for them the clamp is a no-op, so the ranges are the scalar ranges.
 *
 *  cambi.c (calculate_c_values) and cambi_avx2.c (calculate_c_values_avx2,
 *  built and tested, no longer dispatched) keep their own upstream copies of
 *  the walk; a change to the walk there must be mirrored here.
 */

#ifndef FEATURE_CAMBI_C_VALUES_FRAME_H_
#define FEATURE_CAMBI_C_VALUES_FRAME_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

#include "cambi.h"

typedef void (*CambiCValuesRow)(float *c_values, const uint16_t *histograms, const uint16_t *image,
                                const uint16_t *mask, int row, int width, ptrdiff_t stride,
                                const uint16_t num_diffs, const uint16_t *tvi_thresholds,
                                uint16_t vlt_luma, const int *diff_weights, const int *all_diffs,
                                const float *reciprocal_lut);

/* One frame's inputs and outputs; image and mask share `stride` (uint16 units). */
typedef struct {
    float *c_values;
    uint16_t *histograms;
    const uint16_t *image;
    const uint16_t *mask;
    const uint16_t *tvi_for_diff;
    const int *diff_weights;
    const int *all_diffs;
    ptrdiff_t stride;
    int width;
    int height;
    uint16_t pad_size;
    uint16_t num_diffs;
    uint16_t vlt_luma;
    uint16_t v_band_base;
    uint16_t v_band_size;
} CambiCValuesFrame;

/* Columns scanned per scan call: CAMBI_SCAN_MASKS masks of 32 columns. */
#define CAMBI_SCAN_MASKS 8
#define CAMBI_SCAN_BLOCK (32 * CAMBI_SCAN_MASKS)

/* Columns [j0, j0 + n) of image row `row`, n <= CAMBI_SCAN_BLOCK: bit b of
 * masks[m] set when column j0 + 32 * m + b is unmasked and its value lies in
 * the scored band, i.e. when the add or subtract helper for that pixel would
 * update the histogram. Writes masks[0 .. (n + 31) / 32). */
typedef void (*CambiScanRow)(const CambiCValuesFrame *f, int row, int j0, int n, uint32_t *masks);

/* The same for the middle slide: a bit is set when uh_slide for that column
 * would update the histogram, i.e. unless neither pixel is in (unmasked and in
 * band) or both are in with the same value. */
typedef void (*CambiScanSlide)(const CambiCValuesFrame *f, int row_sub, int row_add, int j0, int n,
                               uint32_t *masks);

typedef struct {
    VmafRangeUpdater inc;
    VmafRangeUpdater dec;
    CambiCValuesRow row;
    CambiScanRow scan_row;
    CambiScanSlide scan_slide;
} CambiCValuesKernels;

/* Index of the lowest set bit of a non-zero mask. BSF on MSVC agrees with
 * TZCNT for every non-zero input, so no BMI feature gate is involved. */
static inline int cambi_ctz32(uint32_t x)
{
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long idx = 0;
    (void)_BitScanForward(&idx, (unsigned long)x);
    return (int)idx;
#else
    return __builtin_ctz(x);
#endif
}

/* The scan predicates for one column, for scan row tails (no masked loads on
 * AVX2 or NEON, and a vector load past the last column could leave the
 * picture). `at` indexes image and mask. In band: unmasked and inside the
 * scored band, the test update_histogram_* makes before touching the
 * histogram. */
static inline bool cambi_column_in_band(const CambiCValuesFrame *f, ptrdiff_t at)
{
    return f->mask[at] && (uint16_t)(f->image[at] - f->v_band_base) < f->v_band_size;
}

/* uh_slide's test: the column changes the histogram unless neither pixel is in
 * band, or both are with the same value. */
static inline bool cambi_column_slide_needed(const CambiCValuesFrame *f, ptrdiff_t at_sub,
                                             ptrdiff_t at_add)
{
    const bool sub_in = cambi_column_in_band(f, at_sub);
    const bool add_in = cambi_column_in_band(f, at_add);
    return (sub_in || add_in) && !(sub_in && add_in && f->image[at_sub] == f->image[at_add]);
}

static FORCE_INLINE int cambi_scan_width(const CambiCValuesFrame *f, int j0)
{
    return MIN(CAMBI_SCAN_BLOCK, f->width - j0);
}

static FORCE_INLINE void cambi_c_values_row(const CambiCValuesFrame *f, CambiCValuesKernels k,
                                            int i)
{
    k.row(f->c_values, f->histograms, f->image, f->mask, i, f->width, f->stride, f->num_diffs,
          f->tvi_for_diff, f->vlt_luma, f->diff_weights, f->all_diffs, reciprocal_lut);
}

/* The histogram update one column contributes to a row step. */
typedef enum {
    CAMBI_COLUMN_FIRST_PASS, /* add image row i */
    CAMBI_COLUMN_ADD,        /* add image row i + pad_size */
    CAMBI_COLUMN_SUBTRACT,   /* remove image row i - pad_size - 1 */
    CAMBI_COLUMN_SLIDE,      /* both of the above (uh_slide) */
} CambiColumnOp;

/* One column's update, through the cambi.h helper the scalar walk calls for
 * it. `op` is a constant at every call site, so the switch folds away. */
static FORCE_INLINE void cambi_c_values_column(const CambiCValuesFrame *f, CambiCValuesKernels k,
                                               CambiColumnOp op, int i, int j)
{
    switch (op) {
    case CAMBI_COLUMN_FIRST_PASS:
        update_histogram_add_edge_first_pass(f->histograms, f->image, f->mask, i, j, f->width,
                                             f->stride, f->pad_size, f->num_diffs, f->v_band_base,
                                             f->v_band_size, k.inc);
        break;
    case CAMBI_COLUMN_ADD:
        update_histogram_add_edge(f->histograms, f->image, f->mask, i, j, f->width, f->stride,
                                  f->pad_size, f->num_diffs, f->v_band_base, f->v_band_size, k.inc);
        break;
    case CAMBI_COLUMN_SUBTRACT:
        update_histogram_subtract_edge(f->histograms, f->image, f->mask, i, j, f->width, f->stride,
                                       f->pad_size, f->num_diffs, f->v_band_base, f->v_band_size,
                                       k.dec);
        break;
    default:
        uh_slide_edge(f->histograms, f->image, f->mask, i, j, f->width, f->stride, f->pad_size,
                      f->v_band_base, f->v_band_size, k.inc, k.dec);
        break;
    }
}

/* Runs `op` for every flagged column of one scanned block: bit b of masks[m]
 * is column j0 + 32 * m + b. */
static FORCE_INLINE void cambi_c_values_flagged(const CambiCValuesFrame *f, CambiCValuesKernels k,
                                                CambiColumnOp op, int i, int j0, int n,
                                                const uint32_t *masks)
{
    for (int m = 0; m < (n + 31) / 32; m++) {
        uint32_t flags = masks[m];
        while (flags) {
            const int j = j0 + 32 * m + cambi_ctz32(flags);
            flags &= flags - 1u;
            cambi_c_values_column(f, k, op, i, j);
        }
    }
}

/* One row step of the walk: scan each block of columns, then update the
 * histogram for the flagged ones. The image row the scan tests is the one the
 * update reads: i for the first pass, i + pad_size to add, i - pad_size - 1 to
 * remove, both for the slide. */
static FORCE_INLINE void cambi_c_values_row_step(const CambiCValuesFrame *f, CambiCValuesKernels k,
                                                 CambiColumnOp op, int i)
{
    const int row_add = op == CAMBI_COLUMN_FIRST_PASS ? i : i + f->pad_size;
    const int row_sub = i - f->pad_size - 1;
    uint32_t masks[CAMBI_SCAN_MASKS] = {0};
    for (int j0 = 0; j0 < f->width; j0 += CAMBI_SCAN_BLOCK) {
        const int n = cambi_scan_width(f, j0);
        if (op == CAMBI_COLUMN_SLIDE) {
            k.scan_slide(f, row_sub, row_add, j0, n, masks);
        } else {
            k.scan_row(f, op == CAMBI_COLUMN_SUBTRACT ? row_sub : row_add, j0, n, masks);
        }
        cambi_c_values_flagged(f, k, op, i, j0, n, masks);
    }
}

/*
 * Runs the whole walk. The caller fills `f` except v_band_base / v_band_size,
 * which are derived here exactly as calculate_c_values() derives them.
 */
static FORCE_INLINE void cambi_calculate_c_values_frame(CambiCValuesFrame *f, CambiCValuesKernels k)
{
    const int v_lo_signed = (int)f->vlt_luma - 3 * (int)f->num_diffs + 1;
    f->v_band_base = v_lo_signed > 0 ? (uint16_t)v_lo_signed : 0;
    f->v_band_size = (uint16_t)(f->tvi_for_diff[f->num_diffs - 1] + 1 - f->v_band_base);

    memset(f->c_values, 0, sizeof(float) * (size_t)f->width * (size_t)f->height);
    memset(f->histograms, 0, (size_t)f->width * (size_t)f->v_band_size * sizeof(uint16_t));

    const int pad = f->pad_size;
    for (int i = 0; i < pad; i++) {
        cambi_c_values_row_step(f, k, CAMBI_COLUMN_FIRST_PASS, i);
    }
    for (int i = 0; i < pad + 1; i++) {
        if (i + pad < f->height)
            cambi_c_values_row_step(f, k, CAMBI_COLUMN_ADD, i);
        cambi_c_values_row(f, k, i);
    }
    for (int i = pad + 1; i < f->height - pad; i++) {
        cambi_c_values_row_step(f, k, CAMBI_COLUMN_SLIDE, i);
        cambi_c_values_row(f, k, i);
    }
    for (int i = f->height - pad; i < f->height; i++) {
        if (i - pad - 1 >= 0)
            cambi_c_values_row_step(f, k, CAMBI_COLUMN_SUBTRACT, i);
        cambi_c_values_row(f, k, i);
    }
}

#endif /* FEATURE_CAMBI_C_VALUES_FRAME_H_ */
