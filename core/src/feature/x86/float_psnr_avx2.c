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

#include <immintrin.h>
#include "float_psnr_avx2.h"

double float_psnr_noise_line_avx2(const float *ref, const float *dis, int w)
{
    double result = 0.0;
    int j = 0;

    for (; j + 8 <= w; j += 8) {
        __m256 r = _mm256_loadu_ps(ref + j);
        __m256 d = _mm256_loadu_ps(dis + j);
        __m256 diff = _mm256_sub_ps(r, d);
        __m256 sq = _mm256_mul_ps(diff, diff);

        _Alignas(32) float tmp[8];
        _mm256_store_ps(tmp, sq);
        for (int k = 0; k < 8; k++)
            result += (double)tmp[k];
    }

    for (; j < w; j++) {
        float diff = ref[j] - dis[j];
        result += (double)(diff * diff);
    }

    return result;
}
