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
 * PU21-local SSIM at an arbitrary data range L.
 *
 * The pu21 extractor needs an SSIM evaluation with data-range L = 256 to match
 * the reference pu21_metric.m (ssim(...,'DynamicRange',256)). The fork's
 * shared iqa_ssim() (backing vmaf_fex_float_ssim) HARDCODES L = 255 and feeds
 * the Netflix GOLDEN assertions; it must NOT be modified or parameterised.
 * This is a self-contained, scalar, single-scale (Gaussian-windowed) SSIM that
 * takes L as an argument, leaving the golden SSIM untouched.
 */

#ifndef VMAF_SRC_FEATURE_PU21_SSIM_H
#define VMAF_SRC_FEATURE_PU21_SSIM_H

#include <stddef.h>

/**
 * @brief Pre-allocated SSIM scratch buffers, owned by the caller.
 *
 * Hot-path discipline (mirrors float_ssim.c): the extractor allocates these
 * ONCE in init() and reuses them for every frame, so extract() does zero
 * per-frame heap traffic for pu21's own buffers. All buffers hold n = w*h
 * floats. The two float working planes ref_f / cmp_f live here too (the
 * encoded double planes are converted into them per frame).
 */
struct pu21_ssim_workspace {
    float *ref_f;         /**< reference plane as float, n entries */
    float *cmp_f;         /**< distorted plane as float, n entries */
    float *ref_mu;        /**< blurred reference mean, n entries */
    float *cmp_mu;        /**< blurred distorted mean, n entries */
    float *ref_sigma_sqd; /**< blurred reference E[x^2], n entries */
    float *cmp_sigma_sqd; /**< blurred distorted E[x^2], n entries */
    float *sigma_both;    /**< blurred cross term E[xy], n entries */
};

/**
 * @brief Allocate every workspace buffer for an n = w*h plane.
 *
 * On failure all partially-acquired buffers are freed and the struct is left
 * fully NULLed, so it is safe to pass to pu21_ssim_workspace_free() again.
 *
 * @param ws workspace to populate (must be non-NULL, fields ignored on entry)
 * @param n  element count per buffer (w*h)
 * @return 0 on success, -1 on allocation failure
 */
int pu21_ssim_workspace_alloc(struct pu21_ssim_workspace *ws, size_t n);

/**
 * @brief Free every workspace buffer. NULL-safe per buffer (free(NULL) no-op).
 * @param ws workspace to release (must be non-NULL)
 */
void pu21_ssim_workspace_free(struct pu21_ssim_workspace *ws);

/**
 * @brief Single-scale Gaussian SSIM on two float planes, with explicit data
 *        range L (the SSIM stabilisation constants are C1=(0.01*L)^2,
 *        C2=(0.03*L)^2).
 *
 * The two planes must be packed tightly (stride == w doubles). Mirrors the
 * fork's iqa scalar SSIM (11x11 Gaussian window, KBND_SYMMETRIC border,
 * zli-nflx flat-region clamp) but with L supplied by the caller rather than
 * hardcoded to 255. Inputs are double (the pu21 encoded planes); the separable
 * Gaussian blur runs in float internally, matching the fork's SSIM kernel.
 *
 * The caller supplies a pre-allocated workspace (see
 * pu21_ssim_workspace_alloc) sized for at least w*h elements; this function
 * performs NO heap allocation, so it is safe to call once per frame from the
 * hot path.
 *
 * @param ws     pre-allocated scratch buffers (>= w*h floats each)
 * @param ref    reference plane, w*h doubles
 * @param dist   distorted plane, w*h doubles
 * @param w      plane width  (must be >= GAUSSIAN_LEN)
 * @param h      plane height (must be >= GAUSSIAN_LEN)
 * @param L      data range (e.g. 256.0 for pu21)
 * @param score  out: mean SSIM over the valid (interior) region
 * @return 0 on success, negative errno on failure
 */
int pu21_compute_ssim(struct pu21_ssim_workspace *ws, const double *ref, const double *dist, int w,
                      int h, double L, double *score);

#endif /* VMAF_SRC_FEATURE_PU21_SSIM_H */
