/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Internal header exposing cambi.c's per-stage helpers to the GPU
 *  twins (integer_cambi_cuda.c, integer_cambi_hip.c).
 *  The Vulkan twin (cambi_vulkan.c, T7-36/ADR-0205) was removed per
 *  ADR-0726 (Vulkan backend dropped).
 *
 *  Background: cambi's pipeline (preprocess → spatial mask → per
 *  scale {decimate → filter_mode → calculate_c_values → pool}) was
 *  internal to cambi.c with everything `static`. The GPU twins need
 *  to call the precision-sensitive `calculate_c_values` from a
 *  different TU, so we expose a slim subset.
 *
 *  The exposed helpers keep their `VmafPicture *` signatures —
 *  GPU twins read back device-produced buffers into a
 *  `VmafPicture`-shaped staging plane, then call these directly.
 *  This avoids a buffer-pair refactor of cambi.c (which would
 *  ripple through every CPU SIMD callsite) while letting the
 *  GPU twin reuse the exact CPU code path on the host residual.
 *
 *  Anyone modifying cambi.c MUST keep these signatures stable or
 *  update all GPU call-sites in the same PR.
 */

#ifndef LIBVMAF_FEATURE_CAMBI_INTERNAL_H_
#define LIBVMAF_FEATURE_CAMBI_INTERNAL_H_

#include <stdbool.h>
#include <stdint.h>

#include "libvmaf/picture.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Number of CAMBI scales (matches cambi.c::NUM_SCALES). */
#define VMAF_CAMBI_NUM_SCALES 5
/* 7×7 spatial-mask filter window (matches cambi.c::MASK_FILTER_SIZE). */
#define VMAF_CAMBI_MASK_FILTER_SIZE 7

/* Range update + derivative callback signatures (mirrors cambi.c
 * internal typedefs). */
typedef void (*VmafCambiRangeUpdater)(uint16_t *arr, int left, int right);
typedef void (*VmafCambiDerivativeCalculator)(const uint16_t *image_data,
                                              uint16_t *derivative_buffer, int width, int height,
                                              int row, int stride);

/* Buffer bundle mirror of cambi.c::CambiBuffers. The GPU twins
 * (CUDA, HIP) allocate these directly (no aligned_malloc helper —
 * device memory + plain malloc serve the host residual). */
typedef struct VmafCambiHostBuffers {
    float *c_values;
    uint32_t *mask_dp;
    uint16_t *c_values_histograms;
    uint16_t *filter_mode_buffer;
    uint16_t *diffs_to_consider;
    uint16_t *tvi_for_diff;
    uint16_t *derivative_buffer;
    int *diff_weights;
    int *all_diffs;
} VmafCambiHostBuffers;

/* ----- functions exported from cambi.c (otherwise file-static) ----- */

/* CPU 7×7 spatial mask: derivative kernel + 7×7 SAT box-sum + threshold
 * compare. Stays bit-exact with the in-tree CPU extractor. */
void vmaf_cambi_get_spatial_mask(const VmafPicture *image, VmafPicture *mask, uint32_t *dp,
                                 uint16_t *derivative_buffer, unsigned width, unsigned height,
                                 VmafCambiDerivativeCalculator derivative_callback);

/* Strict 2× subsample (NOT a 2x2 box). */
void vmaf_cambi_decimate(VmafPicture *image, unsigned width, unsigned height);

/* Separable 3-tap mode filter (horizontal + vertical) over `image`. */
void vmaf_cambi_filter_mode(const VmafPicture *image, int width, int height, uint16_t *buffer);

/* Sliding-histogram c-values pass — the precision-sensitive sequential
 * stage that ADR-0205 keeps on host for v1. */
void vmaf_cambi_calculate_c_values(VmafPicture *pic, const VmafPicture *mask_pic, float *c_values,
                                   uint16_t *histograms, uint16_t window_size,
                                   const uint16_t num_diffs, const uint16_t *tvi_for_diff,
                                   uint16_t vlt_luma, const int *diff_weights, const int *all_diffs,
                                   int width, int height, VmafCambiRangeUpdater inc_range_callback,
                                   VmafCambiRangeUpdater dec_range_callback);

/* Top-K spatial pooling. Mutates `c_values` in place via quick-select. */
double vmaf_cambi_spatial_pooling(float *c_values, double topk, unsigned width, unsigned height);

/* Per-scale weight × 16/8/4/2/1 normalisation. */
double vmaf_cambi_weight_scores_per_scale(double *scores_per_scale, uint16_t normalization);

/* (2 * (window/2) + 1)^2 — divisor for the per-scale score weighting. */
uint16_t vmaf_cambi_get_pixels_in_window(uint16_t window_length);

/* Default (CPU scalar) callback bindings. SIMD-dispatch is hidden
 * inside the CPU extractor; the GPU twin always uses the scalar
 * derivative since most of the work happens on the device. */
void vmaf_cambi_default_callbacks(VmafCambiRangeUpdater *inc, VmafCambiRangeUpdater *dec,
                                  VmafCambiDerivativeCalculator *deriv);

/* Source preprocess: decimate-or-resize to enc_width × enc_height,
 * shift to 10-bit, optional anti-dither. Mirrors the CPU
 * cambi_preprocessing() entry. */
int vmaf_cambi_preprocessing(const VmafPicture *image, VmafPicture *preprocessed, int width,
                             int height, int enc_bitdepth);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_FEATURE_CAMBI_INTERNAL_H_ */
