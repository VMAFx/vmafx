/**
 *
 *  Copyright 2016-2020 Netflix, Inc.
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

#include <stddef.h>

/*
 * compute_vif: main float VIF computation over 4 scales.
 *
 * precomputed_filters: optional array of 4 pre-computed Gaussian filters
 *   (one per scale, each up to 128 floats).  Passing non-NULL skips the
 *   per-scale transcendental call to vif_get_filter() — Win #3 of ADR-0500.
 *   precomputed_filter_widths: corresponding filter widths.  Must be non-NULL
 *   if precomputed_filters is non-NULL.
 *   Passing NULL for both restores the original per-call vif_get_filter() path.
 */
int compute_vif(const float *ref, const float *dis, int w, int h, int ref_stride, int dis_stride,
                double *score, double *score_num, double *score_den, double *scores,
                double vif_enhn_gain_limit, double vif_kernelscale, int vif_skip_scale0,
                double vif_sigma_nsq, const float (*precomputed_filters)[128],
                const int *precomputed_filter_widths);
