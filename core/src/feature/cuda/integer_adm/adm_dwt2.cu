/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
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

#ifndef DEVICE_CODE
#include "feature_collector.h"
#endif
#include "cuda/integer_adm_cuda.h"

#include "common.h"

#include <vector>

#include "cuda_helper.cuh"

/* Device helpers are called by nobody but the kernels of this translation
 * unit, so they live in an anonymous namespace (internal linkage). Only the
 * `extern "C"` kernels keep external linkage: the host resolves them by name
 * through cuModuleGetFunction (ADR-0747). */
namespace
{

// Calculates and returns vector with indices for dwt, if upper limit is
// reached, the indices will be mirrored
__device__ __forceinline__ int4 calculate_indices(const int n, const int upper_limit)
{
    int4 indices = make_int4(2 * n - 1, 2 * n, 2 * n + 1, 2 * n + 2);

    if (!n) {
        indices.x = 1;
        indices.y = 0;
        indices.z = 1;
        indices.w = 2;
    }

    if (indices.x >= upper_limit)
        indices.x = (2 * upper_limit - indices.x - 1);

    if (indices.y >= upper_limit)
        indices.y = (2 * upper_limit - indices.y - 1);

    if (indices.z >= upper_limit)
        indices.z = (2 * upper_limit - indices.z - 1);

    if (indices.w >= upper_limit)
        indices.w = (2 * upper_limit - indices.w - 1);

    return indices;
}

template <int32_t add_shift, int16_t shift, typename T>
__device__ __forceinline__ void
dwt_s123_combined_vert_kernel(const T *d_image_scale, int32_t *tmplo_start, int w, int h,
                              int img_stride, AdmFixedParametersCuda params)
{
    const int idx = threadIdx.x + blockIdx.x * blockDim.x;
    const int i = blockIdx.y;
    if (idx >= w)
        return;

    const int32_t *filter_lo = params.dwt2_db2_coeffs_lo;
    const int32_t *filter_hi = params.dwt2_db2_coeffs_hi;

    int64_t accum_lo = 0;
    int64_t accum_hi = 0;
    int32_t *const tmplo = tmplo_start + (static_cast<ptrdiff_t>(w) * i * 2);
    int32_t *const tmphi = tmplo + w;

    const int4 pixel = calculate_indices(i, h);
    const T s10 = d_image_scale[pixel.x * img_stride + idx];
    const T s11 = d_image_scale[pixel.y * img_stride + idx];
    const T s12 = d_image_scale[pixel.z * img_stride + idx];
    const T s13 = d_image_scale[pixel.w * img_stride + idx];

    accum_lo += (int64_t)filter_lo[0] * s10;
    accum_lo += (int64_t)filter_lo[1] * s11;
    accum_lo += (int64_t)filter_lo[2] * s12;
    accum_lo += (int64_t)filter_lo[3] * s13;
    tmplo[idx] = (int32_t)((accum_lo + add_shift) >> shift);

    accum_hi += (int64_t)filter_hi[0] * s10;
    accum_hi += (int64_t)filter_hi[1] * s11;
    accum_hi += (int64_t)filter_hi[2] * s12;
    accum_hi += (int64_t)filter_hi[3] * s13;
    tmphi[idx] = (int32_t)((accum_hi + add_shift) >> shift);
}

template <int32_t add_shift, int16_t shift>
__device__ __forceinline__ void
dwt_s123_combined_hori_kernel(cuda_i4_adm_dwt_band_t i4_dwt2, const int32_t *tmplo_start, int w,
                              int dst_stride, AdmFixedParametersCuda params)
{
    const int idx = threadIdx.x + blockIdx.x * blockDim.x;
    const int i = blockIdx.y;
    if (idx >= (w + 1) / 2)
        return;

    const int32_t *filter_lo = params.dwt2_db2_coeffs_lo;
    const int32_t *filter_hi = params.dwt2_db2_coeffs_hi;

    int64_t accum = 0;

    const int4 pixels = calculate_indices(idx, w);
    const int32_t *const tmplo = tmplo_start + (static_cast<ptrdiff_t>(w) * i * 2);
    const int32_t *const tmphi = tmplo + w;

    const int32_t s0_lo = tmplo[pixels.x];
    const int32_t s1_lo = tmplo[pixels.y];
    const int32_t s2_lo = tmplo[pixels.z];
    const int32_t s3_lo = tmplo[pixels.w];

    const int32_t s0_hi = tmphi[pixels.x];
    const int32_t s1_hi = tmphi[pixels.y];
    const int32_t s2_hi = tmphi[pixels.z];
    const int32_t s3_hi = tmphi[pixels.w];

    accum = 0;
    accum += (int64_t)filter_lo[0] * s0_lo;
    accum += (int64_t)filter_lo[1] * s1_lo;
    accum += (int64_t)filter_lo[2] * s2_lo;
    accum += (int64_t)filter_lo[3] * s3_lo;
    i4_dwt2.band_a[i * dst_stride + idx] = (int32_t)((accum + add_shift) >> shift);

    accum = 0;
    accum += (int64_t)filter_hi[0] * s0_lo;
    accum += (int64_t)filter_hi[1] * s1_lo;
    accum += (int64_t)filter_hi[2] * s2_lo;
    accum += (int64_t)filter_hi[3] * s3_lo;
    i4_dwt2.band_v[i * dst_stride + idx] = (int32_t)((accum + add_shift) >> shift);

    accum = 0;
    accum += (int64_t)filter_lo[0] * s0_hi;
    accum += (int64_t)filter_lo[1] * s1_hi;
    accum += (int64_t)filter_lo[2] * s2_hi;
    accum += (int64_t)filter_lo[3] * s3_hi;
    i4_dwt2.band_h[i * dst_stride + idx] = (int32_t)((accum + add_shift) >> shift);

    accum = 0;
    accum += (int64_t)filter_hi[0] * s0_hi;
    accum += (int64_t)filter_hi[1] * s1_hi;
    accum += (int64_t)filter_hi[2] * s2_hi;
    accum += (int64_t)filter_hi[3] * s3_hi;
    i4_dwt2.band_d[i * dst_stride + idx] = (int32_t)((accum + add_shift) >> shift);
}

/* Accumulator of the scale-0 vertical pass. The first three low-pass taps add
 * up to 50582, so a 16-bit sum passes INT32_MAX once three samples reach
 * 42456; it is formed in int64, as the CPU's adm_dwt2_vpass16_tap4() does. An
 * 8-bit sum always fits in int32. The normalised value fits in int32 either
 * way, so both give the CPU's result for in-range input. */
template <typename T> struct DwtVertAccum {
    using type = int32_t;
};
template <> struct DwtVertAccum<uint16_t> {
    using type = int64_t;
};

/* Four-tap response of shared-tile samples. They are int16, so the sum fits
 * in int32, as in the CPU's adm_dwt2_hpass(). */
__device__ __forceinline__ int32_t dwt_tap4(const int32_t *filter, int32_t s0, int32_t s1,
                                            int32_t s2, int32_t s3)
{
    int32_t accum = 0;
    accum += filter[0] * s0;
    accum += filter[1] * s1;
    accum += filter[2] * s2;
    accum += filter[3] * s3;
    return accum;
}

/* Loads the 2 + 2 * v_rows_per_thread source rows of column x that one
 * thread's vertical outputs read, mirrored at the top and bottom edges. */
template <int v_rows_per_thread, typename T>
__device__ __forceinline__ void adm_dwt2_load_column(const T *d_picture, int x, int y_out, int h,
                                                     int src_stride,
                                                     int32_t (&u_s)[2 + 2 * v_rows_per_thread])
{
    const int read_backwards_elements = 1;
    const int y_in_start = (2 * y_out) - read_backwards_elements;

#pragma unroll
    for (int i = 0; i < 2 + 2 * v_rows_per_thread; ++i) {
        int y_in = y_in_start + i;

        // mirror bottom
        y_in = y_in - max(0, 2 * (y_in - h) + 1);

        // mirror top, only required for read_backwards_elements elements.
        if (i < read_backwards_elements)
            y_in = abs(y_in);

        // no bounds check required due to the mirroring
        u_s[i] = d_picture[y_in * src_stride + x];
    }
}

/* Vertical pass of the fused kernel: v_rows_per_thread outputs of one
 * column into the shared tile, low-pass in .x and high-pass in .y. */
template <int v_rows_per_thread, int32_t tile_width, int tile_height, typename T>
__device__ __forceinline__ void
adm_dwt2_vert_tile(const T *d_picture, short2 (*s_tile)[tile_width], int w, int h, int src_stride,
                   int16_t v_shift, int32_t v_add_shift, AdmFixedParametersCuda params)
{
    using accum_t = typename DwtVertAccum<T>::type;
    const int horz_out_tile_rows = tile_height;
    const int horz_out_tile_cols = tile_width / 2 - 2;

    int x = threadIdx.x + blockIdx.x * 2 * horz_out_tile_cols;
    // For each output at position x of the horizontal pass we have to read the range [2*x-1, 2*x+2]. Thus start one element to the left
    x = max(0, x - 1); /* Deferred: abs(x - 1) would fold the left-edge mirror into this
                        * line; requires auditing the horizontal-pass tile overlap to ensure
                        * the mirrored read is within the shared-memory window. */

    const int y_out =
        (threadIdx.y + blockIdx.y * horz_out_tile_rows / v_rows_per_thread) * v_rows_per_thread;
    if (x >= w)
        return;

    const int32_t *filter_lo = params.dwt2_db2_coeffs_lo;
    const int32_t *filter_hi = params.dwt2_db2_coeffs_hi;

    // we need 2 addition rows for each item processed by a thread.
    int32_t u_s[2 + 2 * v_rows_per_thread];
    adm_dwt2_load_column<v_rows_per_thread>(d_picture, x, y_out, h, src_stride, u_s);

    // accumulate items_per_thread values and store them
#pragma unroll
    for (int item = 0; item < v_rows_per_thread; ++item) {
        // stop for the first row outside of the image
        if ((y_out + item) >= h) {
            break;
        }

        accum_t accum_lo = 0;
        accum_t accum_hi = 0;
#pragma unroll
        for (int i = 0; i < 4; ++i) {
            accum_lo += (accum_t)filter_lo[i] * u_s[i + 2 * item];
            accum_hi += (accum_t)filter_hi[i] * u_s[i + 2 * item];
        }

        /* normalizing is done for range from(0 to N) to (-N/2 to N/2) */
        accum_lo -= (accum_t)params.dwt2_db2_coeffs_lo_sum * v_add_shift;
        accum_hi -= (accum_t)params.dwt2_db2_coeffs_hi_sum * v_add_shift;

        s_tile[threadIdx.y * v_rows_per_thread + item][threadIdx.x] =
            make_short2((short)((accum_lo + v_add_shift) >> v_shift),
                        (short)((accum_hi + v_add_shift) >> v_shift));
    }
}

/* Horizontal pass of the fused kernel: v_rows_per_thread output rows of one
 * column from the shared tile into the four bands. */
template <int v_rows_per_thread, int32_t h_shift, int32_t h_add_shift, int32_t tile_width,
          int tile_height>
__device__ __forceinline__ void adm_dwt2_hori_tile(const short2 (*s_tile)[tile_width],
                                                   cuda_adm_dwt_band_t dst,
                                                   cuda_i4_adm_dwt_band_t i4_dwt2, int w, int h,
                                                   int dst_stride, AdmFixedParametersCuda params)
{
    const int horz_out_tile_rows = tile_height;
    const int horz_out_tile_cols = tile_width / 2 - 2;

    // only ~50% of the threads in the x direction have work to do
    if (threadIdx.x >= horz_out_tile_cols)
        return;

    const int x_out = threadIdx.x + blockIdx.x * horz_out_tile_cols;
    const int x_out_cta = blockIdx.x * horz_out_tile_cols;
    if (x_out >= (w + 1) / 2)
        return;

    const int32_t *filter_lo = params.dwt2_db2_coeffs_lo;
    const int32_t *filter_hi = params.dwt2_db2_coeffs_hi;
    const int4 pixel = calculate_indices(x_out, w);

    // tile columns of the four taps
    const int c0 = pixel.x - 2 * x_out_cta + 1;
    const int c1 = pixel.y - 2 * x_out_cta + 1;
    const int c2 = pixel.z - 2 * x_out_cta + 1;
    const int c3 = pixel.w - 2 * x_out_cta + 1;

    for (int y_thread = 0; y_thread < v_rows_per_thread; ++y_thread) {
        const int y_out =
            threadIdx.y * v_rows_per_thread + y_thread + blockIdx.y * horz_out_tile_rows;
        if (y_out >= (h + 1) / 2)
            return;

        const short2 *const tmp = s_tile[threadIdx.y * v_rows_per_thread + y_thread];
        const int at = y_out * dst_stride + x_out;

        const int32_t band_a =
            (dwt_tap4(filter_lo, tmp[c0].x, tmp[c1].x, tmp[c2].x, tmp[c3].x) + h_add_shift) >>
            h_shift;
        dst.band_a[at] = band_a;
        i4_dwt2.band_a[at] = band_a;
        dst.band_v[at] =
            (dwt_tap4(filter_hi, tmp[c0].x, tmp[c1].x, tmp[c2].x, tmp[c3].x) + h_add_shift) >>
            h_shift;
        dst.band_h[at] =
            (dwt_tap4(filter_lo, tmp[c0].y, tmp[c1].y, tmp[c2].y, tmp[c3].y) + h_add_shift) >>
            h_shift;
        dst.band_d[at] =
            (dwt_tap4(filter_hi, tmp[c0].y, tmp[c1].y, tmp[c2].y, tmp[c3].y) + h_add_shift) >>
            h_shift;
    }
}

/* The fused kernel writes the result of the vertical dwt2 to shared memory for
 * consumption by the horizontal dwt2. The vertical pass computes a tile of size
 * [tile_width, tile_height]. The horizontal pass computes a tile of size
 * [tile_width / 2 - 2, tile_height] due to the required overlap of the tiles. */
template <int v_rows_per_thread, int32_t h_shift, int32_t h_add_shift, int32_t tile_width,
          int tile_height, typename T>
__device__ __forceinline__ void
adm_dwt2_8_vert_hori_kernel(const T *d_picture, cuda_adm_dwt_band_t dst,
                            cuda_i4_adm_dwt_band_t i4_dwt2, int w, int h, int src_stride,
                            int dst_stride, int16_t v_shift, int32_t v_add_shift,
                            AdmFixedParametersCuda params)
{
    __shared__ short2 s_tile[tile_height][tile_width];

    adm_dwt2_vert_tile<v_rows_per_thread, tile_width, tile_height>(
        d_picture, s_tile, w, h, src_stride, v_shift, v_add_shift, params);
    __syncthreads();
    adm_dwt2_hori_tile<v_rows_per_thread, h_shift, h_add_shift, tile_width, tile_height>(
        s_tile, dst, i4_dwt2, w, h, dst_stride, params);
}

} // namespace

// C __global__ KERNEL C DEFINES
#pragma region

#define DWT_S123_COMBINED_VERT(add_shift, shift, type)                                             \
    __global__ void dwt_s123_combined_vert_kernel_##add_shift##_##shift##_##type(                  \
        const type *d_image_scale, int32_t *tmplo_start, int w, int h, int img_stride,             \
        AdmFixedParametersCuda params)                                                             \
    {                                                                                              \
        dwt_s123_combined_vert_kernel<add_shift, shift, type>(d_image_scale, tmplo_start, w, h,    \
                                                              img_stride, params);                 \
    }

/* The unnamed parameter is unused by the kernel but stays in the signature:
 * the host launches it with a fixed `void *args[]` layout. */
#define DWT_S123_COMBINED_HORI(add_shift, shift)                                                   \
    __global__ void dwt_s123_combined_hori_kernel_##add_shift##_##shift(                           \
        const cuda_i4_adm_dwt_band_t i4_dwt2, int32_t *tmplo_start, int w, int /* h */,            \
        int dst_stride, AdmFixedParametersCuda params)                                             \
    {                                                                                              \
        dwt_s123_combined_hori_kernel<add_shift, shift>(i4_dwt2, tmplo_start, w, dst_stride,       \
                                                        params);                                   \
    }

#define DWT_8_VERT_HORI(v_rows_per_thread, h_shift, h_add_shift, tile_width, tile_height, type)                          \
    __global__ void                                                                                                      \
    adm_dwt2_8_vert_hori_kernel_##v_rows_per_thread##_##h_shift##_##h_add_shift##_##tile_width##_##tile_height##_##type( \
        const type *d_picture, cuda_adm_dwt_band_t dst, cuda_i4_adm_dwt_band_t i4_dwt2, int w,                           \
        int h, int src_stride, int dst_stride, int16_t v_shift, int32_t v_add_shift,                                     \
        AdmFixedParametersCuda params)                                                                                   \
    {                                                                                                                    \
        adm_dwt2_8_vert_hori_kernel<v_rows_per_thread, h_shift, h_add_shift, tile_width,                                 \
                                    tile_height, type>(d_picture, dst, i4_dwt2, w, h, src_stride,                        \
                                                       dst_stride, v_shift, v_add_shift, params);                        \
    }
#pragma endregion

extern "C" {
DWT_S123_COMBINED_VERT(0, 0, int32_t);      // dwt_s123_combined_vert_kernel_0_0_int32_t
DWT_S123_COMBINED_VERT(32768, 16, int32_t); // dwt_s123_combined_vert_kernel_32768_16_int32_t
DWT_S123_COMBINED_HORI(16384, 15);          // dwt_s123_combined_hori_kernel_16384_15
DWT_S123_COMBINED_HORI(32768, 16);          // dwt_s123_combined_hori_kernel_32768_16
DWT_8_VERT_HORI(4, 16, 32768, 128, 8,
                uint8_t); // adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint8_t
DWT_8_VERT_HORI(4, 16, 32768, 128, 8,
                uint16_t); // adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint16_t
}
