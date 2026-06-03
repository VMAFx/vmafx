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

#ifndef VMAF_MS_SSIM_DECIMATE_H_INCLUDED
#define VMAF_MS_SSIM_DECIMATE_H_INCLUDED

/*
 * MS-SSIM 2x decimate with the 9-tap 9/7 biorthogonal wavelet LPF.
 *
 * Replaces `iqa_decimate(..., factor=2, &lpf_9x9, ...)` for the
 * MS-SSIM hot path. The 2-D 9x9 kernel in ms_ssim.c (`g_lpf`) is
 * replaced by an equivalent separable form using `g_lpf_h` /
 * `g_lpf_v` (IEEE-754 FMA accumulation, KBND_SYMMETRIC border).
 *
 * Numerical contract: the output MAY differ from `iqa_decimate` by
 * up to a few ULPs per pixel because (a) `g_lpf` is precomputed at
 * 6-decimal precision rather than the exact outer product of
 * `g_lpf_h` x `g_lpf_v`, and (b) separable summation order differs
 * from the 2-D row-major order (IEEE-754 non-associativity). The
 * Netflix MS-SSIM golden-gate assertions absorb this shift at
 * `places=4` tolerance (~5e-5); see ADR-0125.
 *
 * This function is the scalar-separable reference. AVX2 / AVX-512
 * specialisations (when available) produce *bit-identical* output to
 * this scalar path; that is the correctness contract tested in
 * libvmaf/test/test_ms_ssim_decimate.c.
 */

/**
 * Downsample an image by factor 2 with the MS-SSIM 9-tap separable LPF.
 *
 * @param src    Source image buffer, w*h floats, row-major.
 * @param w      Source image width (must be > 0).
 * @param h      Source image height (must be > 0).
 * @param dst    Destination buffer. Must be distinct from `src` and
 *               sized at least (w/2 + w%2) * (h/2 + h%2) floats.
 * @param rw     Optional out: destination width.
 * @param rh     Optional out: destination height.
 * @return 0 on success, -ENOMEM on allocation failure.
 */
int ms_ssim_decimate_scalar(const float *src, int w, int h, float *dst, int *rw, int *rh);

/**
 * Auto-dispatching entry point: picks AVX2 / AVX-512 / scalar based on
 * runtime CPU capability. Output is byte-identical across all three
 * implementations (see libvmaf/test/test_ms_ssim_decimate.c).
 *
 * Signature identical to ms_ssim_decimate_scalar.
 */
int ms_ssim_decimate(const float *src, int w, int h, float *dst, int *rw, int *rh);

#endif /* VMAF_MS_SSIM_DECIMATE_H_INCLUDED */
