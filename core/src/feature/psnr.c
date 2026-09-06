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

#include <math.h>
#include <stdbool.h>
/* ptrdiff_t, used by the widened index casts the CodeQL cast fix added
 * to the inner loop below. Do not prune. */
#include <stddef.h>

#include "psnr.h"

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

int compute_psnr(const float *ref, const float *dis, int w, int h, int ref_stride, int dis_stride,
                 double *score, double peak, double psnr_max, bool uncapped)
{
    double noise_ = 0;

    int ref_stride_ = ref_stride / sizeof(float);
    int dis_stride_ = dis_stride / sizeof(float);

    for (int i = 0; i < h; ++i) {
        for (int j = 0; j < w; ++j) {
            float ref_ = ref[(ptrdiff_t)i * ref_stride_ + j];
            float dis_ = dis[(ptrdiff_t)i * dis_stride_ + j];
            float diff = ref_ - dis_;
            noise_ += diff * diff;
        }
    }
    noise_ /= ((double)w * h);

    /* Split `psnr_max`'s infinity-sentinel role from its truncation role —
     * see psnr.h and ADR-1193. The `!uncapped` arm is the pre-fix
     * expression verbatim, so the default is bit-identical. */
    double eps = 1e-10;
    if (!uncapped) {
        *score = MIN(10 * log10(peak * peak / MAX(noise_, eps)), psnr_max);
    } else if (noise_ <= 0.0) {
        *score = psnr_max;
    } else {
        *score = 10 * log10(peak * peak / MAX(noise_, eps));
    }

    return 0;
}
