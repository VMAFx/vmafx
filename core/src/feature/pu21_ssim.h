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

#ifndef __VMAF_SRC_FEATURE_PU21_SSIM_H__
#define __VMAF_SRC_FEATURE_PU21_SSIM_H__

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
 * @param ref    reference plane, w*h doubles
 * @param dist   distorted plane, w*h doubles
 * @param w      plane width  (must be >= GAUSSIAN_LEN)
 * @param h      plane height (must be >= GAUSSIAN_LEN)
 * @param L      data range (e.g. 256.0 for pu21)
 * @param score  out: mean SSIM over the valid (interior) region
 * @return 0 on success, negative errno on failure
 */
int pu21_compute_ssim(const double *ref, const double *dist, int w, int h, double L, double *score);

#endif /* __VMAF_SRC_FEATURE_PU21_SSIM_H__ */
