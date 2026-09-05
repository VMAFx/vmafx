/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#ifndef __VMAF_PERCENTILE_H__
#define __VMAF_PERCENTILE_H__

#include <math.h>

/* Order-statistic helpers shared by the bootstrap confidence intervals
 * (`core/src/predict.c`) and the percentile temporal-pooling methods
 * (`core/src/libvmaf.c`).
 *
 * Deliberately header-only `static inline` rather than a separate translation
 * unit: `vmaf_percentile` computes the golden-asserted bootstrap `ci_p95`
 * bounds, so the interpolation must keep compiling inside predict.c's own
 * translation unit with the identical expression. A cross-TU call would let a
 * non-LTO build and an LTO build (ADR-1172 turns `-flto` on for release) form
 * the `a*b + c*d` contraction differently and drift the last ULP of a number
 * the Netflix golden gate pins. `static inline` gives each caller a private
 * copy of the same expression instead. See ADR-1188.
 */

/* qsort() comparator ordering doubles ascending. NaN-free input assumed: the
 * feature collector rejects non-finite scores before they reach pooling. */
static inline int vmaf_score_compare(const void *a, const void *b)
{
    const double *x = (const double *)a;
    const double *y = (const double *)b;
    if (*x > *y) {
        return 1;
    }
    if (*x < *y) {
        return -1;
    }
    return 0;
}

/* Linear-interpolated percentile over `scores`, which MUST already be sorted
 * ascending (see vmaf_score_compare) and MUST hold at least one element.
 *
 * The rank is `perc * (n - 1) / 100` and the value is interpolated linearly
 * between the two neighbouring ranks — identical to
 * `numpy.percentile(scores, perc, method="linear")`, which is what the Python
 * harness (`ListStats.perc10` and friends) applies to the same per-frame
 * vector. Keeping the two rules identical is what lets the C API and the
 * Python harness report the same pooled number for the same frames. */
static inline double vmaf_percentile(const double *scores, unsigned n_scores, double perc)
{
    if (n_scores == 0) {
        return 0.;
    }

    const double p = perc * (n_scores - 1) / 100.;
    const int idx_l = (int)floor(p);
    const int idx_r = (int)ceil(p);

    return (idx_l == idx_r) ? scores[idx_l] :
                              scores[idx_l] * (idx_r - p) + scores[idx_r] * (p - idx_l);
}

#endif /* __VMAF_PERCENTILE_H__ */
