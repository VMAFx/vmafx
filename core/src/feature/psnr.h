/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#ifndef PSNR_H_
#define PSNR_H_

#include <stdbool.h>

/*
 * Float-plane PSNR helper of the upstream "tools" layer. `psnr_max` is the
 * finite stand-in reported when the two planes are identical (true PSNR is
 * +inf). `uncapped == false` additionally truncates every computed value at
 * `psnr_max`, which is the historical behaviour; `uncapped == true` reports
 * the true value and keeps only the zero-noise sentinel.
 * See ADR-1193 / T-UPSTREAM-1109 / Netflix/vmaf#1109.
 */
int compute_psnr(const float *ref, const float *dis, int w, int h, int ref_stride, int dis_stride,
                 double *score, double peak, double psnr_max, bool uncapped);

#endif /* PSNR_H_ */
