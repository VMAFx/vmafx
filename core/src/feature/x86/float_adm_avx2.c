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
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include "float_adm_avx2.h"
#include "mem.h"

static const int FLOAT_ABS_MASK_I = 0x7FFFFFFF;

/* adm_csf_s() intentionally evaluates FLOAT_ONE_BY_30 as a double literal
 * before storing the result in a float.  Widen each lane for that multiply so
 * the dispatched kernel preserves the scalar fallback's rounding contract. */
static inline __m256 float_adm_mul_float_by_double_avx2(__m256 value, __m256d factor)
{
    const __m128 value_lo = _mm256_castps256_ps128(value);
    const __m128 value_hi = _mm256_extractf128_ps(value, 1);
    const __m128 result_lo = _mm256_cvtpd_ps(_mm256_mul_pd(_mm256_cvtps_pd(value_lo), factor));
    const __m128 result_hi = _mm256_cvtpd_ps(_mm256_mul_pd(_mm256_cvtps_pd(value_hi), factor));
    return _mm256_insertf128_ps(_mm256_castps128_ps256(result_lo), result_hi, 1);
}

static const float dwt2_db2_coeffs_lo[4] = {0.482962913144690f, 0.836516303737469f,
                                            0.224143868041857f, -0.129409522550921f};

static const float dwt2_db2_coeffs_hi[4] = {-0.129409522550921f, -0.224143868041857f,
                                            0.836516303737469f, -0.482962913144690f};

static void float_adm_dwt2_vertical_row_avx2(const float *const rows[4], float *tmplo, float *tmphi,
                                             int w)
{
    const __m256 vlo0 = _mm256_set1_ps(dwt2_db2_coeffs_lo[0]);
    const __m256 vlo1 = _mm256_set1_ps(dwt2_db2_coeffs_lo[1]);
    const __m256 vlo2 = _mm256_set1_ps(dwt2_db2_coeffs_lo[2]);
    const __m256 vlo3 = _mm256_set1_ps(dwt2_db2_coeffs_lo[3]);
    const __m256 vhi0 = _mm256_set1_ps(dwt2_db2_coeffs_hi[0]);
    const __m256 vhi1 = _mm256_set1_ps(dwt2_db2_coeffs_hi[1]);
    const __m256 vhi2 = _mm256_set1_ps(dwt2_db2_coeffs_hi[2]);
    const __m256 vhi3 = _mm256_set1_ps(dwt2_db2_coeffs_hi[3]);

    int j = 0;
    for (; j + 8 <= w; j += 8) {
        const __m256 s0 = _mm256_loadu_ps(rows[0] + j);
        const __m256 s1 = _mm256_loadu_ps(rows[1] + j);
        const __m256 s2 = _mm256_loadu_ps(rows[2] + j);
        const __m256 s3 = _mm256_loadu_ps(rows[3] + j);
        __m256 lo_acc = _mm256_mul_ps(s0, vlo0);
        lo_acc = _mm256_add_ps(lo_acc, _mm256_mul_ps(s1, vlo1));
        lo_acc = _mm256_add_ps(lo_acc, _mm256_mul_ps(s2, vlo2));
        lo_acc = _mm256_add_ps(lo_acc, _mm256_mul_ps(s3, vlo3));
        _mm256_storeu_ps(tmplo + j, lo_acc);
        __m256 hi_acc = _mm256_mul_ps(s0, vhi0);
        hi_acc = _mm256_add_ps(hi_acc, _mm256_mul_ps(s1, vhi1));
        hi_acc = _mm256_add_ps(hi_acc, _mm256_mul_ps(s2, vhi2));
        hi_acc = _mm256_add_ps(hi_acc, _mm256_mul_ps(s3, vhi3));
        _mm256_storeu_ps(tmphi + j, hi_acc);
    }
    for (; j < w; ++j) {
        const float s0 = rows[0][j];
        const float s1 = rows[1][j];
        const float s2 = rows[2][j];
        const float s3 = rows[3][j];
        tmplo[j] = dwt2_db2_coeffs_lo[0] * s0 + dwt2_db2_coeffs_lo[1] * s1 +
                   dwt2_db2_coeffs_lo[2] * s2 + dwt2_db2_coeffs_lo[3] * s3;
        tmphi[j] = dwt2_db2_coeffs_hi[0] * s0 + dwt2_db2_coeffs_hi[1] * s1 +
                   dwt2_db2_coeffs_hi[2] * s2 + dwt2_db2_coeffs_hi[3] * s3;
    }
}

static void float_adm_dwt2_horizontal_row_avx2(const adm_dwt_band_t_s *dst, int **ind_x, int row,
                                               int w, int dst_px_stride, const float *tmplo,
                                               const float *tmphi)
{
    for (int j = 0; j < (w + 1) / 2; ++j) {
        const int j0 = ind_x[0][j];
        const int j1 = ind_x[1][j];
        const int j2 = ind_x[2][j];
        const int j3 = ind_x[3][j];
        const float sl0 = tmplo[j0];
        const float sl1 = tmplo[j1];
        const float sl2 = tmplo[j2];
        const float sl3 = tmplo[j3];
        dst->band_a[row * dst_px_stride + j] =
            dwt2_db2_coeffs_lo[0] * sl0 + dwt2_db2_coeffs_lo[1] * sl1 +
            dwt2_db2_coeffs_lo[2] * sl2 + dwt2_db2_coeffs_lo[3] * sl3;
        dst->band_v[row * dst_px_stride + j] =
            dwt2_db2_coeffs_hi[0] * sl0 + dwt2_db2_coeffs_hi[1] * sl1 +
            dwt2_db2_coeffs_hi[2] * sl2 + dwt2_db2_coeffs_hi[3] * sl3;
        const float sh0 = tmphi[j0];
        const float sh1 = tmphi[j1];
        const float sh2 = tmphi[j2];
        const float sh3 = tmphi[j3];
        dst->band_h[row * dst_px_stride + j] =
            dwt2_db2_coeffs_lo[0] * sh0 + dwt2_db2_coeffs_lo[1] * sh1 +
            dwt2_db2_coeffs_lo[2] * sh2 + dwt2_db2_coeffs_lo[3] * sh3;
        dst->band_d[row * dst_px_stride + j] =
            dwt2_db2_coeffs_hi[0] * sh0 + dwt2_db2_coeffs_hi[1] * sh1 +
            dwt2_db2_coeffs_hi[2] * sh2 + dwt2_db2_coeffs_hi[3] * sh3;
    }
}

int float_adm_dwt2_avx2(const float *src, const adm_dwt_band_t_s *dst, int **ind_y, int **ind_x,
                        int w, int h, int src_stride, int dst_stride)
{
    const int src_px_stride = src_stride / sizeof(float);
    const int dst_px_stride = dst_stride / sizeof(float);

    float *tmplo = aligned_malloc(ALIGN_CEIL(sizeof(float) * w), MAX_ALIGN);
    float *tmphi = aligned_malloc(ALIGN_CEIL(sizeof(float) * w), MAX_ALIGN);
    if (!tmplo || !tmphi) {
        aligned_free(tmplo);
        aligned_free(tmphi);
        return -ENOMEM;
    }

    for (int i = 0; i < (h + 1) / 2; ++i) {
        const float *rows[4] = {src + (ptrdiff_t)ind_y[0][i] * src_px_stride,
                                src + (ptrdiff_t)ind_y[1][i] * src_px_stride,
                                src + (ptrdiff_t)ind_y[2][i] * src_px_stride,
                                src + (ptrdiff_t)ind_y[3][i] * src_px_stride};
        float_adm_dwt2_vertical_row_avx2(rows, tmplo, tmphi, w);
        float_adm_dwt2_horizontal_row_avx2(dst, ind_x, i, w, dst_px_stride, tmplo, tmphi);
    }

    aligned_free(tmplo);
    aligned_free(tmphi);
    return 0;
}

void float_adm_csf_avx2(const float *src, float *dst, float *flt, int w, int h, int src_stride,
                        int dst_stride, float factor, double one_by_30)
{
    const int src_px_stride = src_stride / sizeof(float);
    const int dst_px_stride = dst_stride / sizeof(float);

    const __m256 abs_mask = _mm256_castsi256_ps(_mm256_set1_epi32(FLOAT_ABS_MASK_I));
    const __m256 vfactor = _mm256_set1_ps(factor);
    const __m256d vone_by_30 = _mm256_set1_pd(one_by_30);

    for (int i = 0; i < h; ++i) {
        int src_offset = i * src_px_stride;
        int dst_offset = i * dst_px_stride;
        int j = 0;

        /* Process 16 floats per iteration (dual accumulators for throughput). */
        for (; j + 16 <= w; j += 16) {
            const __m256 s0 = _mm256_loadu_ps(src + src_offset + j);
            const __m256 s1 = _mm256_loadu_ps(src + src_offset + j + 8);

            const __m256 d0 = _mm256_mul_ps(vfactor, s0);
            const __m256 d1 = _mm256_mul_ps(vfactor, s1);

            _mm256_storeu_ps(dst + dst_offset + j, d0);
            _mm256_storeu_ps(dst + dst_offset + j + 8, d1);

            const __m256 a0 = _mm256_and_ps(d0, abs_mask);
            const __m256 a1 = _mm256_and_ps(d1, abs_mask);

            const __m256 f0 = float_adm_mul_float_by_double_avx2(a0, vone_by_30);
            const __m256 f1 = float_adm_mul_float_by_double_avx2(a1, vone_by_30);

            _mm256_storeu_ps(flt + dst_offset + j, f0);
            _mm256_storeu_ps(flt + dst_offset + j + 8, f1);
        }

        /* Process 8 floats. */
        for (; j + 8 <= w; j += 8) {
            const __m256 s = _mm256_loadu_ps(src + src_offset + j);
            const __m256 d = _mm256_mul_ps(vfactor, s);
            _mm256_storeu_ps(dst + dst_offset + j, d);

            const __m256 a = _mm256_and_ps(d, abs_mask);
            const __m256 f = float_adm_mul_float_by_double_avx2(a, vone_by_30);
            _mm256_storeu_ps(flt + dst_offset + j, f);
        }

        /* Scalar tail. */
        for (; j < w; ++j) {
            const float dst_val = factor * src[src_offset + j];
            dst[dst_offset + j] = dst_val;
            flt[dst_offset + j] = one_by_30 * fabsf(dst_val);
        }
    }
}

float float_adm_csf_den_scale_avx2(const float *src, int w, int h, int src_stride, int left,
                                   int top, int right, int bottom, float factor)
{
    (void)w;
    (void)h;
    int src_px_stride = src_stride / sizeof(float);

    __m256 abs_mask = _mm256_castsi256_ps(_mm256_set1_epi32(FLOAT_ABS_MASK_I));
    __m256 vfactor = _mm256_set1_ps(factor);

    float accum = 0.0f;

    for (int i = top; i < bottom; ++i) {
        const float *row = src + (ptrdiff_t)i * src_px_stride;
        float row_accum = 0.0f;
        int j = left;

        for (; j + 8 <= right; j += 8) {
            __m256 s = _mm256_loadu_ps(row + j);
            __m256 v = _mm256_and_ps(_mm256_mul_ps(vfactor, s), abs_mask);
            __m256 vsq = _mm256_mul_ps(v, v);
            __m256 vcube = _mm256_mul_ps(vsq, v);

            float lanes[8];
            _mm256_storeu_ps(lanes, vcube);
            for (int lane = 0; lane < 8; ++lane)
                row_accum += lanes[lane];
        }

        for (; j < right; ++j) {
            float val = fabsf(factor * row[j]);
            row_accum += val * val * val;
        }

        accum += row_accum;
    }

    return (float)accum;
}
