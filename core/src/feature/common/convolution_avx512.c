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

/*
 * AVX-512F port of the separable float convolution scanlines.
 *
 * Mirrors convolution_avx.c (AVX2) but widens the inner FMA loop from
 * 8 floats (one __m256) to 16 floats (one __m512) per iteration.
 * Reduction order within a scanline chunk differs from AVX2 (wider
 * partial-sum tree), so results are NOT bit-identical to the AVX2 path
 * at the ULP level — this is the standard SIMD-widening trade-off for
 * the float VIF path, documented in ADR-0504 and consistent with the
 * precedent set by ADR-0214 for the integer VIF path.
 *
 * The three public wrappers (convolution_f32_avx512_s,
 * convolution_f32_avx512_sq_s, convolution_f32_avx512_xy_s) have the
 * same signature as their AVX2 counterparts in convolution_avx.c so the
 * dispatch in vif_tools.c / convolution.c is a drop-in replacement.
 *
 * Invariants shared with convolution_avx.c (ADR-0143):
 *   - Scanline helpers carry `static` linkage (no other TU uses them).
 *   - Inner-loop strides use `ptrdiff_t` to avoid int * int UB.
 *   - Unaligned load (_mm512_loadu_ps) for the horizontal pass source
 *     (no alignment guarantee on tmp row interior).
 *   - Unaligned load/store for the vertical pass source/tmp. The project
 *     allocator contract is 32-byte alignment (MAX_ALIGN), while AVX-512
 *     aligned memory ops require 64-byte alignment.
 *   - MAX_FWIDTH_AVX_CONV guard is enforced by the caller; VLA-sized
 *     filter register array avoids stack-blown runtime sizes.
 */

#include <immintrin.h>
#include <stddef.h>

#include "alignment.h"
#include "convolution.h"
#include "convolution_internal.h"

#define AVX512_STEP (16)

/* Horizontal scanline: load 16 floats unaligned (no alignment promise on
 * the interior of a tmp row), multiply-accumulate filter taps, store
 * unaligned to dst with radius offset. */
static void convolution_f32_avx512_s_1d_h_scanline(const float *RESTRICT filter, int filter_width,
                                                   const float *RESTRICT src, float *RESTRICT dst,
                                                   int j_end)
{
    int radius = filter_width / 2;

    __m512 f[MAX_FWIDTH_AVX_CONV];

    for (int k = 0; k < filter_width; k++) {
        f[k] = _mm512_set1_ps(filter[k]);
    }

    for (int j = 0; j < j_end; j += AVX512_STEP) {
        __m512 sum = _mm512_setzero_ps();

        for (int k = 0; k < filter_width; k++) {
            __m512 g = _mm512_loadu_ps(src + j + k);
            sum = _mm512_fmadd_ps(f[k], g, sum);
        }

        _mm512_storeu_ps(dst + j + radius, sum);
    }
}

/* Vertical scanline: source/tmp rows are only guaranteed 32-byte aligned
 * by MAX_ALIGN, so use unaligned AVX-512 memory ops. */
static void convolution_f32_avx512_s_1d_v_scanline(const float *RESTRICT filter, int filter_width,
                                                   const float *RESTRICT src, float *RESTRICT dst,
                                                   ptrdiff_t src_stride, int j_end)
{
    int radius = filter_width / 2;

    src -= (ptrdiff_t)radius * src_stride;

    __m512 f[MAX_FWIDTH_AVX_CONV];

    for (int k = 0; k < filter_width; k++) {
        f[k] = _mm512_set1_ps(filter[k]);
    }

    for (int j = 0; j < j_end; j += AVX512_STEP) {
        __m512 sum = _mm512_setzero_ps();

        for (int k = 0; k < filter_width; k++) {
            __m512 g = _mm512_loadu_ps(src + (ptrdiff_t)k * src_stride + j);
            sum = _mm512_fmadd_ps(f[k], g, sum);
        }

        _mm512_storeu_ps(dst + j, sum);
    }
}

/* Vertical squared scanline: squares the source pixel before the
 * multiply-accumulate, matching convolution_f32_avx_s_1d_v_sq_scanline. */
static void convolution_f32_avx512_s_1d_v_sq_scanline(const float *RESTRICT filter,
                                                      int filter_width, const float *RESTRICT src,
                                                      float *RESTRICT dst, ptrdiff_t src_stride,
                                                      int j_end)
{
    int radius = filter_width / 2;

    src -= (ptrdiff_t)radius * src_stride;

    __m512 f[MAX_FWIDTH_AVX_CONV];

    for (int k = 0; k < filter_width; k++) {
        f[k] = _mm512_set1_ps(filter[k]);
    }

    for (int j = 0; j < j_end; j += AVX512_STEP) {
        __m512 sum = _mm512_setzero_ps();

        for (int k = 0; k < filter_width; k++) {
            __m512 g = _mm512_loadu_ps(src + (ptrdiff_t)k * src_stride + j);
            g = _mm512_mul_ps(g, g);
            sum = _mm512_fmadd_ps(f[k], g, sum);
        }

        _mm512_storeu_ps(dst + j, sum);
    }
}

/* Vertical cross-product scanline: multiplies two source images
 * pixel-wise before the multiply-accumulate, matching
 * convolution_f32_avx_s_1d_v_xy_scanline. */
static void convolution_f32_avx512_s_1d_v_xy_scanline(const float *RESTRICT filter,
                                                      int filter_width, const float *RESTRICT src1,
                                                      const float *RESTRICT src2,
                                                      float *RESTRICT dst, ptrdiff_t src1_stride,
                                                      ptrdiff_t src2_stride, int j_end)
{
    int radius = filter_width / 2;

    src1 -= (ptrdiff_t)radius * src1_stride;
    src2 -= (ptrdiff_t)radius * src2_stride;

    __m512 f[MAX_FWIDTH_AVX_CONV];

    for (int k = 0; k < filter_width; k++) {
        f[k] = _mm512_set1_ps(filter[k]);
    }

    for (int j = 0; j < j_end; j += AVX512_STEP) {
        __m512 sum = _mm512_setzero_ps();

        for (int k = 0; k < filter_width; k++) {
            __m512 g = _mm512_loadu_ps(src1 + (ptrdiff_t)k * src1_stride + j);
            __m512 g2 = _mm512_loadu_ps(src2 + (ptrdiff_t)k * src2_stride + j);
            g = _mm512_mul_ps(g, g2);
            sum = _mm512_fmadd_ps(f[k], g, sum);
        }

        _mm512_storeu_ps(dst + j, sum);
    }
}

/*
 * Public wrappers — same structure as convolution_f32_avx_s / _sq_s / _xy_s
 * in convolution_avx.c, with AVX512_STEP replacing AVX_STEP.
 *
 * tmp_stride is rounded up to AVX512_STEP (16 floats = 64 bytes), but the
 * base pointers are only guaranteed 32-byte aligned by MAX_ALIGN. Keep
 * vertical-pass memory ops unaligned unless the project-wide allocation
 * contract changes.
 */

void convolution_f32_avx512_s(const float *RESTRICT filter, int filter_width,
                              const float *RESTRICT src, float *RESTRICT dst, float *RESTRICT tmp,
                              int width, int height, int src_stride, int dst_stride)
{
    int radius = filter_width / 2;
    int width_floor_step = vmaf_floorn(width, AVX512_STEP);
    int tmp_stride = vmaf_ceiln(width, AVX512_STEP);

    /* Clamp the vertical border split to the plane -- rationale and the
     * negative-`height - radius` failure mode are documented on
     * convolution_clamp_borders() in convolution_internal.h. */
    int i_border_top = radius;
    int i_vec_end = height - radius;
    convolution_clamp_borders(height, &i_border_top, &i_vec_end);
    int j_vec_end = vmaf_floorn(width - radius, AVX512_STEP);

    const ptrdiff_t src_pdt = (ptrdiff_t)src_stride;
    const ptrdiff_t dst_pdt = (ptrdiff_t)dst_stride;
    const ptrdiff_t tmp_pdt = (ptrdiff_t)tmp_stride;

    /* Vertical pass. */
    for (int i = 0; i < i_border_top; ++i) {
        for (int j = 0; j < width; ++j) {
            tmp[(ptrdiff_t)i * tmp_pdt + j] = convolution_edge_s(false, filter, filter_width, src,
                                                                 width, height, src_stride, i, j);
        }
    }
    for (int i = radius; i < i_vec_end; ++i) {
        convolution_f32_avx512_s_1d_v_scanline(filter, filter_width, src + (ptrdiff_t)i * src_pdt,
                                               tmp + (ptrdiff_t)i * tmp_pdt, src_pdt,
                                               width_floor_step);

        for (int j = width_floor_step; j < width; ++j) {
            tmp[(ptrdiff_t)i * tmp_pdt + j] = convolution_edge_s(false, filter, filter_width, src,
                                                                 width, height, src_stride, i, j);
        }
    }
    for (int i = i_vec_end; i < height; ++i) {
        for (int j = 0; j < width; ++j) {
            tmp[(ptrdiff_t)i * tmp_pdt + j] = convolution_edge_s(false, filter, filter_width, src,
                                                                 width, height, src_stride, i, j);
        }
    }

    /* Horizontal pass. */
    for (int i = 0; i < height; ++i) {
        for (int j = 0; j < radius; ++j) {
            dst[(ptrdiff_t)i * dst_pdt + j] = convolution_edge_s(true, filter, filter_width, tmp,
                                                                 width, height, tmp_stride, i, j);
        }

        convolution_f32_avx512_s_1d_h_scanline(filter, filter_width, tmp + (ptrdiff_t)i * tmp_pdt,
                                               dst + (ptrdiff_t)i * dst_pdt, j_vec_end);

        for (int j = j_vec_end; j < width; ++j) {
            dst[(ptrdiff_t)i * dst_pdt + j] = convolution_edge_s(true, filter, filter_width, tmp,
                                                                 width, height, tmp_stride, i, j);
        }
    }
}

void convolution_f32_avx512_sq_s(const float *RESTRICT filter, int filter_width,
                                 const float *RESTRICT src, float *RESTRICT dst,
                                 float *RESTRICT tmp, int width, int height, int src_stride,
                                 int dst_stride)
{
    int radius = filter_width / 2;
    int width_floor_step = vmaf_floorn(width, AVX512_STEP);
    int tmp_stride = vmaf_ceiln(width, AVX512_STEP);

    /* Clamp the vertical border split to the plane -- rationale and the
     * negative-`height - radius` failure mode are documented on
     * convolution_clamp_borders() in convolution_internal.h. */
    int i_border_top = radius;
    int i_vec_end = height - radius;
    convolution_clamp_borders(height, &i_border_top, &i_vec_end);
    int j_vec_end = vmaf_floorn(width - radius, AVX512_STEP);

    const ptrdiff_t src_pdt = (ptrdiff_t)src_stride;
    const ptrdiff_t dst_pdt = (ptrdiff_t)dst_stride;
    const ptrdiff_t tmp_pdt = (ptrdiff_t)tmp_stride;

    /* Vertical pass. */
    for (int i = 0; i < i_border_top; ++i) {
        for (int j = 0; j < width; ++j) {
            tmp[(ptrdiff_t)i * tmp_pdt + j] = convolution_edge_sq_s(
                false, filter, filter_width, src, width, height, src_stride, i, j);
        }
    }
    for (int i = radius; i < i_vec_end; ++i) {
        convolution_f32_avx512_s_1d_v_sq_scanline(
            filter, filter_width, src + (ptrdiff_t)i * src_pdt, tmp + (ptrdiff_t)i * tmp_pdt,
            src_pdt, width_floor_step);

        for (int j = width_floor_step; j < width; ++j) {
            tmp[(ptrdiff_t)i * tmp_pdt + j] = convolution_edge_sq_s(
                false, filter, filter_width, src, width, height, src_stride, i, j);
        }
    }
    for (int i = i_vec_end; i < height; ++i) {
        for (int j = 0; j < width; ++j) {
            tmp[(ptrdiff_t)i * tmp_pdt + j] = convolution_edge_sq_s(
                false, filter, filter_width, src, width, height, src_stride, i, j);
        }
    }

    /* Horizontal pass. */
    for (int i = 0; i < height; ++i) {
        for (int j = 0; j < radius; ++j) {
            dst[(ptrdiff_t)i * dst_pdt + j] = convolution_edge_s(true, filter, filter_width, tmp,
                                                                 width, height, tmp_stride, i, j);
        }

        convolution_f32_avx512_s_1d_h_scanline(filter, filter_width, tmp + (ptrdiff_t)i * tmp_pdt,
                                               dst + (ptrdiff_t)i * dst_pdt, j_vec_end);

        for (int j = j_vec_end; j < width; ++j) {
            dst[(ptrdiff_t)i * dst_pdt + j] = convolution_edge_s(true, filter, filter_width, tmp,
                                                                 width, height, tmp_stride, i, j);
        }
    }
}

void convolution_f32_avx512_xy_s(const float *RESTRICT filter, int filter_width,
                                 const float *RESTRICT src1, const float *RESTRICT src2,
                                 float *RESTRICT dst, float *RESTRICT tmp, int width, int height,
                                 int src1_stride, int src2_stride, int dst_stride)
{
    int radius = filter_width / 2;
    int width_floor_step = vmaf_floorn(width, AVX512_STEP);
    int tmp_stride = vmaf_ceiln(width, AVX512_STEP);

    /* Clamp the vertical border split to the plane -- rationale and the
     * negative-`height - radius` failure mode are documented on
     * convolution_clamp_borders() in convolution_internal.h. */
    int i_border_top = radius;
    int i_vec_end = height - radius;
    convolution_clamp_borders(height, &i_border_top, &i_vec_end);
    int j_vec_end = vmaf_floorn(width - radius, AVX512_STEP);

    const ptrdiff_t src1_pdt = (ptrdiff_t)src1_stride;
    const ptrdiff_t src2_pdt = (ptrdiff_t)src2_stride;
    const ptrdiff_t dst_pdt = (ptrdiff_t)dst_stride;
    const ptrdiff_t tmp_pdt = (ptrdiff_t)tmp_stride;

    /* Vertical pass. */
    for (int i = 0; i < i_border_top; ++i) {
        for (int j = 0; j < width; ++j) {
            tmp[(ptrdiff_t)i * tmp_pdt + j] =
                convolution_edge_xy_s(false, filter, filter_width, src1, src2, width, height,
                                      src1_stride, src2_stride, i, j);
        }
    }
    for (int i = radius; i < i_vec_end; ++i) {
        convolution_f32_avx512_s_1d_v_xy_scanline(
            filter, filter_width, src1 + (ptrdiff_t)i * src1_pdt, src2 + (ptrdiff_t)i * src2_pdt,
            tmp + (ptrdiff_t)i * tmp_pdt, src1_pdt, src2_pdt, width_floor_step);

        for (int j = width_floor_step; j < width; ++j) {
            tmp[(ptrdiff_t)i * tmp_pdt + j] =
                convolution_edge_xy_s(false, filter, filter_width, src1, src2, width, height,
                                      src1_stride, src2_stride, i, j);
        }
    }
    for (int i = i_vec_end; i < height; ++i) {
        for (int j = 0; j < width; ++j) {
            tmp[(ptrdiff_t)i * tmp_pdt + j] =
                convolution_edge_xy_s(false, filter, filter_width, src1, src2, width, height,
                                      src1_stride, src2_stride, i, j);
        }
    }

    /* Horizontal pass. */
    for (int i = 0; i < height; ++i) {
        for (int j = 0; j < radius; ++j) {
            dst[(ptrdiff_t)i * dst_pdt + j] = convolution_edge_s(true, filter, filter_width, tmp,
                                                                 width, height, tmp_stride, i, j);
        }

        convolution_f32_avx512_s_1d_h_scanline(filter, filter_width, tmp + (ptrdiff_t)i * tmp_pdt,
                                               dst + (ptrdiff_t)i * dst_pdt, j_vec_end);

        for (int j = j_vec_end; j < width; ++j) {
            dst[(ptrdiff_t)i * dst_pdt + j] = convolution_edge_s(true, filter, filter_width, tmp,
                                                                 width, height, tmp_stride, i, j);
        }
    }
}
