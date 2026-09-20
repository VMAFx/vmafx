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
#include <string.h>

#include "feature/adm_tools.h"
#include "mem.h"
#include "float_adm_avx512.h"

static inline __m512 avx512_abs_ps(__m512 v)
{
    const __m512i mask = _mm512_set1_epi32(0x7FFFFFFF);
    return _mm512_castsi512_ps(_mm512_and_si512(_mm512_castps_si512(v), mask));
}

/* See float_adm_mul_float_by_double_avx2(): this widening is required for
 * exact agreement with adm_csf_s(), whose scale literal has type double. */
static inline __m512 float_adm_mul_float_by_double_avx512(__m512 value, __m512d factor)
{
    const __m256 value_lo = _mm512_castps512_ps256(value);
    const __m256 value_hi = _mm512_extractf32x8_ps(value, 1);
    const __m256 result_lo = _mm512_cvtpd_ps(_mm512_mul_pd(_mm512_cvtps_pd(value_lo), factor));
    const __m256 result_hi = _mm512_cvtpd_ps(_mm512_mul_pd(_mm512_cvtps_pd(value_hi), factor));
    return _mm512_insertf32x8(_mm512_castps256_ps512(result_lo), result_hi, 1);
}

static const float dwt2_db2_coeffs_lo[4] = {0.482962913144690f, 0.836516303737469f,
                                            0.224143868041857f, -0.129409522550921f};
static const float dwt2_db2_coeffs_hi[4] = {-0.129409522550921f, -0.224143868041857f,
                                            0.836516303737469f, -0.482962913144690f};

typedef struct Dwt2FiltersAvx512 {
    __m512 lo[4];
    __m512 hi[4];
} Dwt2FiltersAvx512;

typedef struct Dwt2BandsAvx512 {
    __m512 lo;
    __m512 hi;
} Dwt2BandsAvx512;

static inline Dwt2FiltersAvx512 float_adm_dwt2_filters_avx512(void)
{
    Dwt2FiltersAvx512 filters;
    for (int tap = 0; tap < 4; ++tap) {
        filters.lo[tap] = _mm512_set1_ps(dwt2_db2_coeffs_lo[tap]);
        filters.hi[tap] = _mm512_set1_ps(dwt2_db2_coeffs_hi[tap]);
    }
    return filters;
}

static void float_adm_dwt2_vertical_row_avx512(const float *const rows[4], float *tmplo,
                                               float *tmphi, int w,
                                               const Dwt2FiltersAvx512 *filters)
{
    int j = 0;
    for (; j + 16 <= w; j += 16) {
        const __m512 s0 = _mm512_loadu_ps(rows[0] + j);
        const __m512 s1 = _mm512_loadu_ps(rows[1] + j);
        const __m512 s2 = _mm512_loadu_ps(rows[2] + j);
        const __m512 s3 = _mm512_loadu_ps(rows[3] + j);
        __m512 lo_acc = _mm512_mul_ps(filters->lo[0], s0);
        lo_acc = _mm512_add_ps(lo_acc, _mm512_mul_ps(filters->lo[1], s1));
        lo_acc = _mm512_add_ps(lo_acc, _mm512_mul_ps(filters->lo[2], s2));
        lo_acc = _mm512_add_ps(lo_acc, _mm512_mul_ps(filters->lo[3], s3));
        _mm512_storeu_ps(tmplo + j, lo_acc);
        __m512 hi_acc = _mm512_mul_ps(filters->hi[0], s0);
        hi_acc = _mm512_add_ps(hi_acc, _mm512_mul_ps(filters->hi[1], s1));
        hi_acc = _mm512_add_ps(hi_acc, _mm512_mul_ps(filters->hi[2], s2));
        hi_acc = _mm512_add_ps(hi_acc, _mm512_mul_ps(filters->hi[3], s3));
        _mm512_storeu_ps(tmphi + j, hi_acc);
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

static inline void float_adm_dwt2_horizontal_scalar_avx512(const adm_dwt_band_t_s *dst, int **ind_x,
                                                           int row, int column, int dst_px_stride,
                                                           const float *tmplo, const float *tmphi)
{
    const int j0 = ind_x[0][column];
    const int j1 = ind_x[1][column];
    const int j2 = ind_x[2][column];
    const int j3 = ind_x[3][column];
    const float sl0 = tmplo[j0];
    const float sl1 = tmplo[j1];
    const float sl2 = tmplo[j2];
    const float sl3 = tmplo[j3];
    const int offset = row * dst_px_stride + column;
    dst->band_a[offset] = dwt2_db2_coeffs_lo[0] * sl0 + dwt2_db2_coeffs_lo[1] * sl1 +
                          dwt2_db2_coeffs_lo[2] * sl2 + dwt2_db2_coeffs_lo[3] * sl3;
    dst->band_v[offset] = dwt2_db2_coeffs_hi[0] * sl0 + dwt2_db2_coeffs_hi[1] * sl1 +
                          dwt2_db2_coeffs_hi[2] * sl2 + dwt2_db2_coeffs_hi[3] * sl3;
    const float sh0 = tmphi[j0];
    const float sh1 = tmphi[j1];
    const float sh2 = tmphi[j2];
    const float sh3 = tmphi[j3];
    dst->band_h[offset] = dwt2_db2_coeffs_lo[0] * sh0 + dwt2_db2_coeffs_lo[1] * sh1 +
                          dwt2_db2_coeffs_lo[2] * sh2 + dwt2_db2_coeffs_lo[3] * sh3;
    dst->band_d[offset] = dwt2_db2_coeffs_hi[0] * sh0 + dwt2_db2_coeffs_hi[1] * sh1 +
                          dwt2_db2_coeffs_hi[2] * sh2 + dwt2_db2_coeffs_hi[3] * sh3;
}

static inline Dwt2BandsAvx512
float_adm_dwt2_horizontal_block_avx512(const float *tmp, int column, __m512i idx_even,
                                       __m512i idx_odd, const Dwt2FiltersAvx512 *filters)
{
    const ptrdiff_t start = (ptrdiff_t)column * 2;
    const __m512 a = _mm512_loadu_ps(tmp + start - 1);
    const __m512 b = _mm512_loadu_ps(tmp + start - 1 + 16);
    const __m512 tap0 = _mm512_permutex2var_ps(a, idx_even, b);
    const __m512 tap1 = _mm512_permutex2var_ps(a, idx_odd, b);
    const __m512 c = _mm512_loadu_ps(tmp + start + 1);
    const __m512 d = _mm512_loadu_ps(tmp + start + 1 + 16);
    const __m512 tap2 = _mm512_permutex2var_ps(c, idx_even, d);
    const __m512 tap3 = _mm512_permutex2var_ps(c, idx_odd, d);
    Dwt2BandsAvx512 bands;
    bands.lo = _mm512_mul_ps(filters->lo[0], tap0);
    bands.lo = _mm512_add_ps(bands.lo, _mm512_mul_ps(filters->lo[1], tap1));
    bands.lo = _mm512_add_ps(bands.lo, _mm512_mul_ps(filters->lo[2], tap2));
    bands.lo = _mm512_add_ps(bands.lo, _mm512_mul_ps(filters->lo[3], tap3));
    bands.hi = _mm512_mul_ps(filters->hi[0], tap0);
    bands.hi = _mm512_add_ps(bands.hi, _mm512_mul_ps(filters->hi[1], tap1));
    bands.hi = _mm512_add_ps(bands.hi, _mm512_mul_ps(filters->hi[2], tap2));
    bands.hi = _mm512_add_ps(bands.hi, _mm512_mul_ps(filters->hi[3], tap3));
    return bands;
}

static void float_adm_dwt2_horizontal_row_avx512(const adm_dwt_band_t_s *dst, int **ind_x, int row,
                                                 int w, int dst_px_stride, const float *tmplo,
                                                 const float *tmphi,
                                                 const Dwt2FiltersAvx512 *filters)
{
    const int half_w = (w + 1) / 2;
    const __m512i idx_even =
        _mm512_set_epi32(30, 28, 26, 24, 22, 20, 18, 16, 14, 12, 10, 8, 6, 4, 2, 0);
    const __m512i idx_odd =
        _mm512_set_epi32(31, 29, 27, 25, 23, 21, 19, 17, 15, 13, 11, 9, 7, 5, 3, 1);
    float_adm_dwt2_horizontal_scalar_avx512(dst, ind_x, row, 0, dst_px_stride, tmplo, tmphi);
    int j = 1;
    for (; j + 16 <= half_w && 2 * j + 32 < w; j += 16) {
        const ptrdiff_t output_offset = (ptrdiff_t)row * dst_px_stride + j;
        const Dwt2BandsAvx512 lo =
            float_adm_dwt2_horizontal_block_avx512(tmplo, j, idx_even, idx_odd, filters);
        _mm512_storeu_ps(dst->band_a + output_offset, lo.lo);
        _mm512_storeu_ps(dst->band_v + output_offset, lo.hi);
        const Dwt2BandsAvx512 hi =
            float_adm_dwt2_horizontal_block_avx512(tmphi, j, idx_even, idx_odd, filters);
        _mm512_storeu_ps(dst->band_h + output_offset, hi.lo);
        _mm512_storeu_ps(dst->band_d + output_offset, hi.hi);
    }
    for (; j < half_w; ++j)
        float_adm_dwt2_horizontal_scalar_avx512(dst, ind_x, row, j, dst_px_stride, tmplo, tmphi);
}

int float_adm_dwt2_avx512(const float *src, const adm_dwt_band_t_s *dst, int **ind_y, int **ind_x,
                          int w, int h, int src_stride, int dst_stride)
{
    const int src_px_stride = src_stride / sizeof(float);
    const int dst_px_stride = dst_stride / sizeof(float);
    float *tmplo = aligned_malloc(ALIGN_CEIL(sizeof(float) * (w + 32)), MAX_ALIGN);
    float *tmphi = aligned_malloc(ALIGN_CEIL(sizeof(float) * (w + 32)), MAX_ALIGN);
    if (!tmplo || !tmphi) {
        aligned_free(tmplo);
        aligned_free(tmphi);
        return -ENOMEM;
    }
    memset(tmplo + w, 0, 32 * sizeof(float));
    memset(tmphi + w, 0, 32 * sizeof(float));
    const Dwt2FiltersAvx512 filters = float_adm_dwt2_filters_avx512();
    for (int i = 0; i < (h + 1) / 2; ++i) {
        const float *rows[4] = {src + (ptrdiff_t)ind_y[0][i] * src_px_stride,
                                src + (ptrdiff_t)ind_y[1][i] * src_px_stride,
                                src + (ptrdiff_t)ind_y[2][i] * src_px_stride,
                                src + (ptrdiff_t)ind_y[3][i] * src_px_stride};
        float_adm_dwt2_vertical_row_avx512(rows, tmplo, tmphi, w, &filters);
        float_adm_dwt2_horizontal_row_avx512(dst, ind_x, i, w, dst_px_stride, tmplo, tmphi,
                                             &filters);
    }

    aligned_free(tmplo);
    aligned_free(tmphi);
    return 0;
}

void float_adm_csf_avx512(const float *src, float *dst, float *flt, int w, int h, int src_stride,
                          int dst_stride, float factor, double one_by_30)
{
    const int src_px_stride = src_stride / sizeof(float);
    const int dst_px_stride = dst_stride / sizeof(float);

    const __m512 vfactor = _mm512_set1_ps(factor);
    const __m512d vone_by_30 = _mm512_set1_pd(one_by_30);

    int i;
    int j;

    for (i = 0; i < h; ++i) {
        const float *src_row = src + (ptrdiff_t)i * src_px_stride;
        float *dst_row = dst + (ptrdiff_t)i * dst_px_stride;
        float *flt_row = flt + (ptrdiff_t)i * dst_px_stride;

        for (j = 0; j + 16 <= w; j += 16) {
            const __m512 sv = _mm512_loadu_ps(src_row + j);
            const __m512 dst_val = _mm512_mul_ps(vfactor, sv);
            _mm512_storeu_ps(dst_row + j, dst_val);
            const __m512 abs_dst = avx512_abs_ps(dst_val);
            const __m512 flt_val = float_adm_mul_float_by_double_avx512(abs_dst, vone_by_30);
            _mm512_storeu_ps(flt_row + j, flt_val);
        }

        /* Scalar tail */
        for (; j < w; ++j) {
            const float dst_val = factor * src_row[j];
            dst_row[j] = dst_val;
            flt_row[j] = one_by_30 * fabsf(dst_val);
        }
    }
}

float float_adm_csf_den_scale_avx512(const float *src, int w, int h, int src_stride, int left,
                                     int top, int right, int bottom, float factor)
{
    (void)w;
    (void)h;
    int src_px_stride = src_stride / sizeof(float);

    __m512 vfactor = _mm512_set1_ps(factor);

    float accum = 0.0f;
    int i;
    int j;

    for (i = top; i < bottom; ++i) {
        const float *row = src + (ptrdiff_t)i * src_px_stride;
        float row_accum = 0.0f;

        for (j = left; j + 16 <= right; j += 16) {
            __m512 sv = _mm512_loadu_ps(row + j);
            __m512 val = avx512_abs_ps(_mm512_mul_ps(vfactor, sv));
            __m512 val2 = _mm512_mul_ps(val, val);
            __m512 val3 = _mm512_mul_ps(val2, val);

            float lanes[16];
            _mm512_storeu_ps(lanes, val3);
            for (int lane = 0; lane < 16; ++lane)
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
