/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
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

#ifndef VMAF_FEATURE_X86_VIF_STATISTIC_AVX2_H_
#define VMAF_FEATURE_X86_VIF_STATISTIC_AVX2_H_

/* Fork-local AVX2 variant of `vif_statistic_s` (upstream ships no AVX2
 * variant of this function). Signature mirrors the scalar
 * [`vif_statistic_s`](../vif_tools.h), including the `vif_sigma_nsq`
 * runtime parameter added in ADR-0142. */
void vif_statistic_s_avx2(const float *mu1, const float *mu2, const float *xx_filt,
                          const float *yy_filt, const float *xy_filt, float *num, float *den, int w,
                          int h, int mu1_stride, int mu2_stride, int xx_filt_stride,
                          int yy_filt_stride, int xy_filt_stride, double vif_enhn_gain_limit,
                          double vif_sigma_nsq);

#endif /* VMAF_FEATURE_X86_VIF_STATISTIC_AVX2_H_ */
