/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Host tail of calc_psnrhvs() (third_party/xiph/psnr_hvs.c) for the GPU twins
 * (ADR-1397). A twin computes the masked coefficient errors of every 8x8
 * block on the device; these helpers turn them into the scores the CPU
 * extractor emits, with its accumulation order, types and rounding points,
 * so that twin and CPU agree bit for bit.
 */

#ifndef FEATURE_PSNR_HVS_SCORE_H_
#define FEATURE_PSNR_HVS_SCORE_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Values calc_psnrhvs() adds to its running sum per 8x8 block. */
#define VMAF_PSNR_HVS_TERMS_PER_BLOCK 64u
/* Deepest input the extractors accept. */
#define VMAF_PSNR_HVS_MAX_BPC 12u

/*
 * Plane score calc_psnrhvs() returns, from the terms it sums.
 *
 * `terms` holds VMAF_PSNR_HVS_TERMS_PER_BLOCK values per block: the blocks in
 * raster order (rows of blocks top to bottom, blocks left to right), the
 * values of one block row-major, each `(err * csf) * (err * csf)` as the CPU
 * computes it. They are added one by one into a single `float`, which is then
 * divided by the term count and by the squared sample maximum, both in
 * `float`. Returns NaN when `terms` is NULL, `n_blocks` is 0 or too large for
 * the CPU's `int` term counter, or `bpc` is outside 1..VMAF_PSNR_HVS_MAX_BPC.
 */
double vmaf_psnr_hvs_plane_score(const float *terms, size_t n_blocks, unsigned bpc);

/*
 * Compacted plane score: sums `n_compact_terms` nonzero terms in CPU order,
 * dividing by the full plane's term count (`n_blocks * VMAF_PSNR_HVS_TERMS_PER_BLOCK`)
 * and squared sample maximum. Bit-identical to vmaf_psnr_hvs_plane_score() when
 * `compact_terms` contains the nonzero values of `terms` in original index order.
 * Returns NaN when n_blocks is 0 or too large, bpc is outside 1..VMAF_PSNR_HVS_MAX_BPC,
 * n_compact_terms > n_blocks * VMAF_PSNR_HVS_TERMS_PER_BLOCK, or compact_terms is NULL
 * while n_compact_terms > 0.
 */
double vmaf_psnr_hvs_plane_score_compacted(const float *compact_terms, size_t n_compact_terms,
                                           size_t n_blocks, unsigned bpc);

/*
 * Linear score behind the combined `psnr_hvs` feature: luma alone for one
 * plane, 0.8 Y + 0.1 (Cb + Cr) for three (the CPU extract() expression).
 * Returns NaN when `plane_scores` is NULL or `n_planes` is neither 1 nor 3.
 */
double vmaf_psnr_hvs_combined_score(const double *plane_scores, unsigned n_planes);

/* convert_score_db(score, 1.0) of the CPU extractor. */
double vmaf_psnr_hvs_score_db(double score);

#ifdef __cplusplus
}
#endif

#endif /* FEATURE_PSNR_HVS_SCORE_H_ */
