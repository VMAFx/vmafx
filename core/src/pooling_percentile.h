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

#ifndef VMAF_POOLING_PERCENTILE_H_
#define VMAF_POOLING_PERCENTILE_H_

#include <assert.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int score_compare(const void *a, const void *b)
{
    assert(a != NULL);
    assert(b != NULL);
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

static inline double percentile(const double *scores, unsigned n_scores, double perc)
{
    assert(scores != NULL);
    assert(n_scores > 0);
    assert(perc >= 0.0 && perc <= 100.0);

    const double p = perc * (n_scores - 1) / 100.;
    const int idx_l = (int)floor(p);
    const int idx_r = (int)ceil(p);

    return (idx_l == idx_r) ? scores[idx_l] :
                              scores[idx_l] * (idx_r - p) + scores[idx_r] * (p - idx_l);
}

#ifdef __cplusplus
}
#endif

#endif /* VMAF_POOLING_PERCENTILE_H_ */
