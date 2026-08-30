/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
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

#ifndef IQA_SSIM_SIMD_H_
#define IQA_SSIM_SIMD_H_

#include <pthread.h>

typedef void (*ssim_precompute_fn)(const float *ref, const float *cmp, float *ref_sq, float *cmp_sq,
                                   float *ref_cmp, int n);

typedef void (*ssim_variance_fn)(float *ref_sigma_sqd, float *cmp_sigma_sqd, float *sigma_both,
                                 const float *ref_mu, const float *cmp_mu, int n);

typedef void (*ssim_accumulate_fn)(const float *ref_mu, const float *cmp_mu,
                                   const float *ref_sigma_sqd, const float *cmp_sigma_sqd,
                                   const float *sigma_both, int n, float C1, float C2, float C3,
                                   double *ssim_sum, double *l_sum, double *c_sum, double *s_sum);

void iqa_ssim_set_dispatch(ssim_precompute_fn precompute, ssim_variance_fn variance,
                           ssim_accumulate_fn accumulate);

/*
 * Fork-local addition: SIMD dispatch for `iqa_convolve` (the 1-D
 * separable, interior-only fast path under IQA_CONVOLVE_1D). The
 * signature is primitive — decouples SIMD translation units from
 * iqa/convolve.h `struct iqa_kernel`. ssim_tools.c adapts the struct
 * to these primitive args at each call site.
 *
 * `workspace` is a caller-owned `w*h`-float scratch buffer used by the
 * horizontal pass. NULL is accepted — the kernel allocates internally
 * (kept for standalone unit tests). The hot path in `iqa_ssim`
 * allocates once and reuses across all 5 dispatch sites, eliminating
 * ~1200 calloc/free pairs per 120-frame run at 1080p. See
 * docs/adr/0138-iqa-convolve-avx2-bitexact-double.md.
 */
typedef void (*iqa_convolve_fn)(float *img, int w, int h, const float *kernel_h,
                                const float *kernel_v, int kw, int kh, int normalized,
                                float *workspace, float *result, int *rw, int *rh);

void iqa_convolve_set_dispatch(iqa_convolve_fn convolve);

/*
 * Thread-safe one-shot installer for the SSIM + iqa_convolve SIMD
 * dispatch tables. The callback is invoked exactly once across the
 * process via pthread_once; subsequent calls return immediately
 * without touching the dispatch globals.
 *
 * Rationale: the dispatch pointers `g_ssim_precompute`,
 * `g_ssim_variance`, `g_ssim_accumulate`, `g_iqa_convolve` are
 * process-wide and were historically installed at feature_extractor
 * init() time. When the libvmaf thread pool runs >=4 worker threads
 * and both float_ssim and float_ms_ssim are enabled, every worker
 * calls its extractor's init() concurrently, racing on those globals.
 * TSan flags the race; ADR-0138 mandates we treat dispatch-install
 * races as real bugs because partial-word tearing on weakly-ordered
 * hardware could install a torn function pointer.
 *
 * Callers pass an ISA-detection callback; it runs once and is
 * guaranteed to complete (with full memory barrier) before any
 * concurrent reader observes the installed function pointers.
 *
 * Each calling translation unit owns its own pthread_once_t guard
 * (passed by pointer). The installer callbacks across TUs install
 * the same ISA-best dispatch and are therefore idempotent; whichever
 * guard fires first wins the install and every subsequent caller —
 * from the same TU or another — short-circuits inside pthread_once.
 */
void iqa_ssim_install_dispatch_once(pthread_once_t *guard, void (*installer)(void));

#endif /* IQA_SSIM_SIMD_H_ */
