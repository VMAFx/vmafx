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

#include "cuda_helper.cuh"
#include "cuda/integer_vif_cuda.h"

#include "common.h"

#include "vif_statistics.cuh"

/*
 * Shared-memory staging for VIF filter passes (perf-audit wins #1 + #4,
 * 2026-05-16).
 *
 * HORIZONTAL PASS (win #1):
 *   Launch: BLOCKX=128, BLOCKY=1, val_per_thread=2.
 *   Each block owns 256 output pixels per row.  The 17-tap filter
 *   (half_fw=8) reads [x_out-8, x_out+8] from each of 7 tmp channels
 *   (mu1, mu2, ref, dis, ref_dis, ref_convol, dis_convol).
 *   Tile: 256+16=272 uint32_t per channel × 7 channels × 4 B = 7616 B.
 *   272 % 32 = 16 → no bank conflicts for warp-consecutive stride-1 access.
 *   Boundary mirror handled in the smem load phase; the compute phase reads
 *   smem unconditionally (interior) or with the same si formula (border).
 *   Estimated speedup: 20–35% on scale-0 VIF horizontal pass.
 *
 * VERTICAL PASS (win #4):
 *   Launch: BLOCKX=32, BLOCKY=4, val_per_thread=4 (uint32_t, 4 uint8_t).
 *   Each block owns 128 cols × 4 rows.  17 vertical taps → tile height 20.
 *   Tile: 2 planes × 128 cols × 20 rows × 1 B = 5120 B (8-bit path).
 *   For 16-bit: 2 planes × 128 cols × 20 rows × 2 B = 10240 B.
 *   Boundary mirror handled in the smem load phase.
 *   Estimated speedup: 15–25% on VIF vertical pass.
 *
 * CORRECTNESS:
 *   All arithmetic is integer fixed-point; smem staging only moves where the
 *   values are read from, not what values are read.  Bit-identical to the
 *   pre-smem implementation.  Verified by cross_backend_parity_gate.py
 *   --features vif --backends cpu cuda --places 4.
 */

/*
 * Horizontal tile width macro.
 * blockx  = number of threads in X per block (= BLOCKX)
 * vpt     = val_per_thread
 * half_fw = fwidth / 2
 * Result: blockx*vpt + 2*half_fw = the minimum span that covers all filter
 *         taps for every thread in the block.
 * +1 pad: avoids stride-32 bank aliasing that could arise with certain
 *         fwidth values (e.g. fwidth=9 → half_fw=4 → tile=256+8=264;
 *         264%32=8, fine, but +1 ensures no future regression).
 */
#define HORI_TILE_W(blockx, vpt, half_fw) ((blockx) * (vpt) + 2 * (half_fw) + 1)

/* -------------------------------------------------------------------------
 * 8-bit VERTICAL KERNEL (win #4)
 * Stages ref_in / dis_in rows into shared memory before the accumulation loop.
 * Block size: BLOCKX=32, BLOCKY=4, val_per_thread=4 (uint32_t alignment).
 * Tile: (BLOCKY + fwidth_0 - 1) rows × (BLOCKX * val_per_thread = 128) cols.
 * For fwidth_0=17: (4+16)=20 rows × 128 cols × 2 planes × 1 B = 5120 B.
 * Conservative static smem size uses BLOCKY_MAX=8 to cover any valid launch.
 * ------------------------------------------------------------------------- */
__device__ __forceinline__ int vif_filter_mirror(int value, int extent)
{
    if (value < 0)
        value = -value;
    if (value >= extent)
        value = 2 * extent - value - 2;
    if (value < 0)
        value = 0;
    if (value >= extent)
        value = extent - 1;
    return value;
}

template <typename alignment_type, typename pixel_type, int fwidth, int val_per_thread>
__device__ __forceinline__ void
vif_load_vertical_tile(const VifBufferCuda &buf, const pixel_type *ref_in, const pixel_type *dis_in,
                       ptrdiff_t stride, int w, int h, pixel_type *ref_tile, pixel_type *dis_tile)
{
    constexpr int tile_cols = 128;
    constexpr int half_fv = fwidth / 2;
    const int x_block_start = blockIdx.x * blockDim.x * val_per_thread;
    const int tile_h = blockDim.y + fwidth - 1;
    const int col = threadIdx.x * val_per_thread;
    for (int tile_row = threadIdx.y; tile_row < tile_h; tile_row += blockDim.y) {
        const int img_row =
            vif_filter_mirror((int)(blockIdx.y * blockDim.y) - half_fv + tile_row, h);
        if (x_block_start + col < w) {
            const alignment_type ref_vec = *reinterpret_cast<const alignment_type *>(
                &ref_in[(ptrdiff_t)img_row * stride + x_block_start + col]);
            const alignment_type dis_vec = *reinterpret_cast<const alignment_type *>(
                &dis_in[(ptrdiff_t)img_row * stride + x_block_start + col]);
            const pixel_type *ref_values = reinterpret_cast<const pixel_type *>(&ref_vec);
            const pixel_type *dis_values = reinterpret_cast<const pixel_type *>(&dis_vec);
#pragma unroll
            for (int k = 0; k < val_per_thread; ++k) {
                ref_tile[tile_row * tile_cols + col + k] = ref_values[k];
                dis_tile[tile_row * tile_cols + col + k] = dis_values[k];
            }
        }
    }
    (void)buf;
}

template <int count> struct VifVertical8Accum {
    __align__(sizeof(uint4)) uint32_t mu1[count];
    __align__(sizeof(uint4)) uint32_t mu2[count];
    __align__(sizeof(uint4)) uint32_t ref[count];
    __align__(sizeof(uint4)) uint32_t dis[count];
    __align__(sizeof(uint4)) uint32_t ref_dis[count];
    __align__(sizeof(uint4)) uint32_t ref_rd[count];
    __align__(sizeof(uint4)) uint32_t dis_rd[count];
};

template <int count, int fwidth, int fwidth_rd>
__device__ __forceinline__ void
vif_accumulate_vertical_8(const uint8_t *ref_tile, const uint8_t *dis_tile, int x_start, int col,
                          int w, filter_table_stuct vif_filt, VifVertical8Accum<count> &sum)
{
    constexpr int tile_cols = 128;
    constexpr int rd_start = (fwidth - fwidth_rd) / 2;
    for (int fi = 0; fi < fwidth; ++fi) {
        const int row_offset = (threadIdx.y + fi) * tile_cols;
        for (int off = 0; off < count; ++off) {
            if (x_start + off < w) {
                const uint32_t coeff = vif_filt.filter[0][fi];
                const uint32_t ref = ref_tile[row_offset + col + off];
                const uint32_t dis = dis_tile[row_offset + col + off];
                const uint32_t coeff_ref = coeff * ref;
                const uint32_t coeff_dis = coeff * dis;
                sum.mu1[off] += coeff_ref;
                sum.mu2[off] += coeff_dis;
                sum.ref[off] += coeff_ref * ref;
                sum.dis[off] += coeff_dis * dis;
                sum.ref_dis[off] += coeff_ref * dis;
                if (fi >= rd_start && fi < fwidth - rd_start) {
                    const uint16_t rd_coeff = vif_filt.filter[1][fi - rd_start];
                    sum.ref_rd[off] += rd_coeff * ref;
                    sum.dis_rd[off] += rd_coeff * dis;
                }
            }
        }
    }
    for (int off = 0; off < count; ++off) {
        sum.mu1[off] = (sum.mu1[off] + 128) >> 8;
        sum.mu2[off] = (sum.mu2[off] + 128) >> 8;
        sum.ref_rd[off] = (sum.ref_rd[off] + 128) >> 8;
        sum.dis_rd[off] = (sum.dis_rd[off] + 128) >> 8;
    }
}

template <int count>
__device__ __forceinline__ void vif_write_vertical(VifBufferCuda buf, int y, int x_start, int w,
                                                   const uint32_t *mu1, const uint32_t *mu2,
                                                   const uint32_t *ref, const uint32_t *dis,
                                                   const uint32_t *ref_dis, const uint32_t *ref_rd,
                                                   const uint32_t *dis_rd, bool write_rd)
{
    using writeback_type = uint4;
    const int stride = buf.stride_tmp / sizeof(uint32_t);
    for (int idx = 0; idx < count; idx += sizeof(writeback_type) / sizeof(uint32_t)) {
        if (x_start + idx < w) {
            const int out = y * stride + x_start + idx;
            *reinterpret_cast<writeback_type *>(&buf.tmp.mu1[out]) =
                *reinterpret_cast<const writeback_type *>(&mu1[idx]);
            *reinterpret_cast<writeback_type *>(&buf.tmp.mu2[out]) =
                *reinterpret_cast<const writeback_type *>(&mu2[idx]);
            *reinterpret_cast<writeback_type *>(&buf.tmp.ref[out]) =
                *reinterpret_cast<const writeback_type *>(&ref[idx]);
            *reinterpret_cast<writeback_type *>(&buf.tmp.dis[out]) =
                *reinterpret_cast<const writeback_type *>(&dis[idx]);
            *reinterpret_cast<writeback_type *>(&buf.tmp.ref_dis[out]) =
                *reinterpret_cast<const writeback_type *>(&ref_dis[idx]);
            if (write_rd) {
                *reinterpret_cast<writeback_type *>(&buf.tmp.ref_convol[out]) =
                    *reinterpret_cast<const writeback_type *>(&ref_rd[idx]);
                *reinterpret_cast<writeback_type *>(&buf.tmp.dis_convol[out]) =
                    *reinterpret_cast<const writeback_type *>(&dis_rd[idx]);
            }
        }
    }
}

template <typename alignment_type = uint2, int fwidth_0 = 17, int fwidth_1 = 9>
__device__ __forceinline__ void filter1d_8_vertical_kernel(VifBufferCuda buf, uint8_t *ref_in,
                                                           uint8_t *dis_in, int w, int h,
                                                           filter_table_stuct vif_filt)
{
    constexpr int count = sizeof(alignment_type);
    static_assert(count % 4 == 0 && count <= 16,
                  "val per thread bust be divisible by 4 and under 16");
    constexpr int tile_rows = 8 + fwidth_0 - 1;
    constexpr int tile_cols = 128;
    __shared__ uint8_t ref_tile[tile_rows][tile_cols];
    __shared__ uint8_t dis_tile[tile_rows][tile_cols];
    vif_load_vertical_tile<alignment_type, uint8_t, fwidth_0, count>(
        buf, ref_in, dis_in, buf.stride, w, h, &ref_tile[0][0], &dis_tile[0][0]);
    __syncthreads();
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const int x_start = (blockIdx.x * blockDim.x + threadIdx.x) * count;
    if (x_start < w && y < h) {
        VifVertical8Accum<count> sum = {};
        const int col = threadIdx.x * count;
        vif_accumulate_vertical_8<count, fwidth_0, fwidth_1>(&ref_tile[0][0], &dis_tile[0][0],
                                                             x_start, col, w, vif_filt, sum);
        vif_write_vertical<count>(buf, y, x_start, w, sum.mu1, sum.mu2, sum.ref, sum.dis,
                                  sum.ref_dis, sum.ref_rd, sum.dis_rd, true);
    }
}

template <int width> struct VifHorizontalTile {
    uint32_t mu1[width];
    uint32_t mu2[width];
    uint32_t ref[width];
    uint32_t dis[width];
    uint32_t ref_dis[width];
    uint32_t ref_convol[width];
    uint32_t dis_convol[width];
};

template <int count> struct VifHorizontalAccum {
    uint32_t mu1[count];
    uint32_t mu2[count];
    uint32_t ref[count];
    uint32_t dis[count];
    uint32_t ref_dis[count];
    uint64_t ref_tmp[count];
    uint64_t dis_tmp[count];
    uint64_t ref_dis_tmp[count];
    uint32_t ref_rd[count / 2];
    uint32_t dis_rd[count / 2];
};

template <int tile_width>
__device__ __forceinline__ void vif_load_horizontal_8(VifBufferCuda buf, int w, int h, int y,
                                                      int tile_x0,
                                                      VifHorizontalTile<tile_width> &tile)
{
    const int row = y * (buf.stride_tmp / sizeof(uint32_t));
    if (y < h) {
        for (int si = threadIdx.x; si < tile_width - 1; si += blockDim.x) {
            const int x = vif_filter_mirror(tile_x0 + si, w);
            tile.mu1[si] = __ldg(&buf.tmp.mu1[row + x]);
            tile.mu2[si] = __ldg(&buf.tmp.mu2[row + x]);
            tile.ref[si] = __ldg(&buf.tmp.ref[row + x]);
            tile.dis[si] = __ldg(&buf.tmp.dis[row + x]);
            tile.ref_dis[si] = __ldg(&buf.tmp.ref_dis[row + x]);
            tile.ref_convol[si] = __ldg(&buf.tmp.ref_convol[row + x]);
            tile.dis_convol[si] = __ldg(&buf.tmp.dis_convol[row + x]);
        }
    }
}

template <int tile_width>
__device__ __forceinline__ void vif_load_horizontal_16(VifBufferCuda buf, int w, int h, int y,
                                                       int tile_x0,
                                                       VifHorizontalTile<tile_width> &tile)
{
    const int row = y * (buf.stride_tmp / sizeof(uint32_t));
    if (y < h) {
        for (int si = threadIdx.x; si < tile_width - 1; si += blockDim.x) {
            const int x = vif_filter_mirror(tile_x0 + si, w);
            tile.mu1[si] = buf.tmp.mu1[row + x];
            tile.mu2[si] = buf.tmp.mu2[row + x];
            tile.ref[si] = buf.tmp.ref[row + x];
            tile.dis[si] = buf.tmp.dis[row + x];
            tile.ref_dis[si] = buf.tmp.ref_dis[row + x];
            tile.ref_convol[si] = buf.tmp.ref_convol[row + x];
            tile.dis_convol[si] = buf.tmp.dis_convol[row + x];
        }
    }
}

template <int count, int fwidth, int fwidth_rd, int scale, int tile_width>
__device__ __forceinline__ void
vif_accumulate_horizontal_interior(const VifHorizontalTile<tile_width> &tile,
                                   filter_table_stuct vif_filt, int base,
                                   VifHorizontalAccum<count> &sum)
{
    constexpr int half = fwidth / 2;
    constexpr int rd_start = (fwidth - fwidth_rd) / 2;
    constexpr int rd_half = fwidth_rd / 2;
    const uint16_t center = vif_filt.filter[scale][half];
#pragma unroll
    for (int off = 0; off < count; ++off) {
        const int si = base + off + half;
        sum.mu1[off] += center * tile.mu1[si];
        sum.mu2[off] += center * tile.mu2[si];
        sum.ref_tmp[off] += center * (uint64_t)tile.ref[si];
        sum.dis_tmp[off] += center * (uint64_t)tile.dis[si];
        sum.ref_dis_tmp[off] += center * (uint64_t)tile.ref_dis[si];
    }
    if (fwidth_rd > 0) {
        const uint32_t rd_center = vif_filt.filter[scale + 1][rd_half];
#pragma unroll
        for (int off = 0; off < count; off += 2) {
            const int si = base + off + half;
            sum.ref_rd[off / 2] += rd_center * tile.ref_convol[si];
            sum.dis_rd[off / 2] += rd_center * tile.dis_convol[si];
        }
    }
#pragma unroll
    for (int fj = 0; fj < half; ++fj) {
        const uint16_t coeff = vif_filt.filter[scale][fj];
#pragma unroll
        for (int off = 0; off < count; ++off) {
            const int lo = base + off + fj;
            const int hi = base + off + 2 * half - fj;
            sum.mu1[off] += coeff * (tile.mu1[lo] + tile.mu1[hi]);
            sum.mu2[off] += coeff * (tile.mu2[lo] + tile.mu2[hi]);
            sum.ref_tmp[off] += coeff * ((uint64_t)tile.ref[lo] + (uint64_t)tile.ref[hi]);
            sum.dis_tmp[off] += coeff * ((uint64_t)tile.dis[lo] + (uint64_t)tile.dis[hi]);
            sum.ref_dis_tmp[off] +=
                coeff * ((uint64_t)tile.ref_dis[lo] + (uint64_t)tile.ref_dis[hi]);
        }
        if (fwidth_rd > 0 && fj >= rd_start && fj < rd_start + rd_half) {
            const uint32_t rd_coeff = vif_filt.filter[scale + 1][fj - rd_start];
#pragma unroll
            for (int off = 0; off < count; off += 2) {
                const int lo = base + off + fj;
                const int hi = base + off + 2 * half - fj;
                sum.ref_rd[off / 2] += rd_coeff * (tile.ref_convol[lo] + tile.ref_convol[hi]);
                sum.dis_rd[off / 2] += rd_coeff * (tile.dis_convol[lo] + tile.dis_convol[hi]);
            }
        }
    }
}

template <int count, int fwidth, int fwidth_rd, int scale, int tile_width>
__device__ __forceinline__ void
vif_accumulate_horizontal_border(const VifHorizontalTile<tile_width> &tile,
                                 filter_table_stuct vif_filt, int base, int x_start, int w,
                                 VifHorizontalAccum<count> &sum)
{
    constexpr int rd_start = (fwidth - fwidth_rd) / 2;
#pragma unroll
    for (int fj = 0; fj < fwidth; ++fj) {
#pragma unroll
        for (int off = 0; off < count; ++off) {
            if (x_start + off < w) {
                const int si = base + off + fj;
                const uint16_t coeff = vif_filt.filter[scale][fj];
                sum.mu1[off] += coeff * tile.mu1[si];
                sum.mu2[off] += coeff * tile.mu2[si];
                sum.ref_tmp[off] += coeff * (uint64_t)tile.ref[si];
                sum.dis_tmp[off] += coeff * (uint64_t)tile.dis[si];
                sum.ref_dis_tmp[off] += coeff * (uint64_t)tile.ref_dis[si];
                if (fwidth_rd > 0 && fj >= rd_start && fj < fwidth - rd_start && off % 2 == 0) {
                    const uint32_t rd_coeff = vif_filt.filter[scale + 1][fj - rd_start];
                    sum.ref_rd[off / 2] += rd_coeff * tile.ref_convol[si];
                    sum.dis_rd[off / 2] += rd_coeff * tile.dis_convol[si];
                }
            }
        }
    }
}

union VifThreadAccum {
    vif_accums values;
    int64_t words[7];
};

template <int count>
__device__ __forceinline__ void vif_finish_horizontal(VifHorizontalAccum<count> &sum, int x_start,
                                                      int w, int h, int32_t round, int32_t shift,
                                                      double gain_limit, vif_accums *accum)
{
    VifThreadAccum thread = {};
    for (int off = 0; off < count; ++off) {
        const int x = x_start + off;
        if (x < w) {
            sum.ref[off] = (uint32_t)((sum.ref_tmp[off] + round) >> shift);
            sum.dis[off] = (uint32_t)((sum.dis_tmp[off] + round) >> shift);
            sum.ref_dis[off] = (uint32_t)((sum.ref_dis_tmp[off] + round) >> shift);
            vif_statistic_calculation<uint32_t>(sum.mu1[off], sum.mu2[off], sum.ref[off],
                                                sum.dis[off], sum.ref_dis[off], x, w, h, gain_limit,
                                                thread.values);
        }
    }
    for (int i = 0; i < 7; ++i)
        thread.words[i] = warp_reduce(thread.words[i]);
    if (threadIdx.x % VMAF_CUDA_THREADS_PER_WARP == 0) {
        for (int i = 0; i < 7; ++i)
            atomicAdd_int64(&reinterpret_cast<int64_t *>(accum)[i], thread.words[i]);
    }
}

template <int count>
__device__ __forceinline__ void vif_write_downsample(VifBufferCuda buf, int x_start, int y, int w,
                                                     int h, const VifHorizontalAccum<count> &sum)
{
    uint16_t *ref = (uint16_t *)buf.ref;
    uint16_t *dis = (uint16_t *)buf.dis;
    const ptrdiff_t stride = buf.rd_stride / sizeof(uint16_t);
    for (int off = 0; off < count; ++off) {
        const int x = x_start + off;
        if (y < h && x < w && y % 2 == 0 && off % 2 == 0) {
            ref[(y / 2) * stride + (x / 2)] = (uint16_t)((sum.ref_rd[off / 2] + 32768) >> 16);
            dis[(y / 2) * stride + (x / 2)] = (uint16_t)((sum.dis_rd[off / 2] + 32768) >> 16);
        }
    }
}

template <int val_per_thread = 1, int fwidth_0 = 17, int fwidth_1 = 9>
__device__ __forceinline__ void filter1d_8_horizontal_kernel(VifBufferCuda buf, int w, int h,
                                                             filter_table_stuct vif_filt,
                                                             double gain_limit, vif_accums *accum)
{
    static_assert(val_per_thread % 2 == 0, "val_per_thread must be divisible by 2");
    constexpr int half = fwidth_0 / 2;
    constexpr int tile_width = HORI_TILE_W(128, val_per_thread, half);
    __shared__ VifHorizontalTile<tile_width> tile;
    const int y = blockIdx.y;
    const int x_start = (blockIdx.x * blockDim.x + threadIdx.x) * val_per_thread;
    const int tile_x0 = (int)(blockIdx.x * blockDim.x) * val_per_thread - half;
    vif_load_horizontal_8(buf, w, h, y, tile_x0, tile);
    __syncthreads();
    if (y < h && x_start < w) {
        VifHorizontalAccum<val_per_thread> sum = {};
        const int base = threadIdx.x * val_per_thread;
        const bool interior = x_start >= half && x_start + val_per_thread - 1 + half < w;
        if (interior)
            vif_accumulate_horizontal_interior<val_per_thread, fwidth_0, fwidth_1, 0>(
                tile, vif_filt, base, sum);
        else
            vif_accumulate_horizontal_border<val_per_thread, fwidth_0, fwidth_1, 0>(
                tile, vif_filt, base, x_start, w, sum);
        vif_finish_horizontal(sum, x_start, w, h, 32768, 16, gain_limit, accum);
        vif_write_downsample(buf, x_start, y, w, h, sum);
    }
}

template <int count> struct VifVertical16Accum {
    __align__(sizeof(uint4)) uint32_t mu1[count];
    __align__(sizeof(uint4)) uint32_t mu2[count];
    __align__(sizeof(uint4)) uint32_t ref_rd[count];
    __align__(sizeof(uint4)) uint32_t dis_rd[count];
    uint64_t ref[count];
    uint64_t dis[count];
    uint64_t ref_dis[count];
    __align__(sizeof(uint4)) uint32_t ref_out[count];
    __align__(sizeof(uint4)) uint32_t dis_out[count];
    __align__(sizeof(uint4)) uint32_t ref_dis_out[count];
};

template <int count, int fwidth, int fwidth_rd, int scale>
__device__ __forceinline__ void
vif_accumulate_vertical_16(const uint16_t *ref_tile, const uint16_t *dis_tile, int x_start, int col,
                           int w, filter_table_stuct vif_filt, VifVertical16Accum<count> &sum)
{
    constexpr int tile_cols = 128;
    constexpr int rd_start = (fwidth - fwidth_rd) / 2;
    for (int fi = 0; fi < fwidth; ++fi) {
        const int row_offset = (threadIdx.y + fi) * tile_cols;
        for (int off = 0; off < count; ++off) {
            if (x_start + off < w) {
                const uint16_t coeff = vif_filt.filter[scale][fi];
                const uint32_t ref = ref_tile[row_offset + col + off];
                const uint32_t dis = dis_tile[row_offset + col + off];
                const uint32_t coeff_ref = coeff * ref;
                const uint32_t coeff_dis = coeff * dis;
                sum.mu1[off] += coeff_ref;
                sum.mu2[off] += coeff_dis;
                sum.ref[off] += coeff_ref * (uint64_t)ref;
                sum.dis[off] += coeff_dis * (uint64_t)dis;
                sum.ref_dis[off] += coeff_ref * (uint64_t)dis;
                if (fwidth_rd > 0 && fi >= rd_start && fi < fwidth - rd_start) {
                    const uint16_t rd_coeff = vif_filt.filter[scale + 1][fi - rd_start];
                    sum.ref_rd[off] += rd_coeff * ref;
                    sum.dis_rd[off] += rd_coeff * dis;
                }
            }
        }
    }
}

template <int count, int fwidth_rd>
__device__ __forceinline__ void
vif_normalize_vertical_16(VifVertical16Accum<count> &sum, int32_t round, int32_t shift,
                          int32_t square_round, int32_t square_shift)
{
    for (int off = 0; off < count; ++off) {
        sum.mu1[off] = (uint16_t)((sum.mu1[off] + round) >> shift);
        sum.mu2[off] = (uint16_t)((sum.mu2[off] + round) >> shift);
        sum.ref_out[off] = (uint32_t)((sum.ref[off] + square_round) >> square_shift);
        sum.dis_out[off] = (uint32_t)((sum.dis[off] + square_round) >> square_shift);
        sum.ref_dis_out[off] = (uint32_t)((sum.ref_dis[off] + square_round) >> square_shift);
        if (fwidth_rd > 0) {
            sum.ref_rd[off] = (uint16_t)((sum.ref_rd[off] + round) >> shift);
            sum.dis_rd[off] = (uint16_t)((sum.dis_rd[off] + round) >> shift);
        }
    }
}

template <typename alignment_type = uint2, int fwidth, int fwidth_rd, int scale>
__device__ __forceinline__ void
filter1d_16_vertical_kernel(VifBufferCuda buf, uint16_t *ref_in, uint16_t *dis_in, int w, int h,
                            int32_t round, int32_t shift, int32_t square_round,
                            int32_t square_shift, filter_table_stuct vif_filt)
{
    constexpr int count = sizeof(alignment_type) / sizeof(uint16_t);
    static_assert(count % 4 == 0 && count <= 8,
                  "val per thread bust be divisible by 4 and under 16");
    constexpr int tile_rows = 8 + fwidth - 1;
    constexpr int tile_cols = 128;
    __shared__ uint16_t ref_tile[tile_rows][tile_cols];
    __shared__ uint16_t dis_tile[tile_rows][tile_cols];
    const ptrdiff_t stride =
        scale == 0 ? buf.stride / sizeof(uint16_t) : buf.rd_stride / sizeof(uint16_t);
    vif_load_vertical_tile<alignment_type, uint16_t, fwidth, count>(
        buf, ref_in, dis_in, stride, w, h, &ref_tile[0][0], &dis_tile[0][0]);
    __syncthreads();
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const int x_start = (blockIdx.x * blockDim.x + threadIdx.x) * count;
    if (x_start < w && y < h) {
        VifVertical16Accum<count> sum = {};
        const int col = threadIdx.x * count;
        vif_accumulate_vertical_16<count, fwidth, fwidth_rd, scale>(
            &ref_tile[0][0], &dis_tile[0][0], x_start, col, w, vif_filt, sum);
        vif_normalize_vertical_16<count, fwidth_rd>(sum, round, shift, square_round, square_shift);
        vif_write_vertical<count>(buf, y, x_start, w, sum.mu1, sum.mu2, sum.ref_out, sum.dis_out,
                                  sum.ref_dis_out, sum.ref_rd, sum.dis_rd, fwidth_rd > 0);
    }
}

template <int val_per_thread = 2, int fwidth, int fwidth_rd, int scale>
__device__ __forceinline__ void
filter1d_16_horizontal_kernel(VifBufferCuda buf, int w, int h, int32_t round, int32_t shift,
                              filter_table_stuct vif_filt, double gain_limit, vif_accums *accum)
{
    static_assert(val_per_thread % 2 == 0, "val_per_thread must be divisible by 2");
    constexpr int half = fwidth / 2;
    constexpr int tile_width = HORI_TILE_W(128, val_per_thread, half);
    __shared__ VifHorizontalTile<tile_width> tile;
    const int y = blockIdx.y;
    const int x_start = (blockIdx.x * blockDim.x + threadIdx.x) * val_per_thread;
    const int tile_x0 = (int)(blockIdx.x * blockDim.x) * val_per_thread - half;
    vif_load_horizontal_16(buf, w, h, y, tile_x0, tile);
    __syncthreads();
    if (x_start < w && y < h) {
        VifHorizontalAccum<val_per_thread> sum = {};
        const int base = threadIdx.x * val_per_thread;
        const bool interior = x_start >= half && x_start + val_per_thread - 1 + half < w;
        if (interior)
            vif_accumulate_horizontal_interior<val_per_thread, fwidth, fwidth_rd, scale>(
                tile, vif_filt, base, sum);
        else
            vif_accumulate_horizontal_border<val_per_thread, fwidth, fwidth_rd, scale>(
                tile, vif_filt, base, x_start, w, sum);
        vif_finish_horizontal(sum, x_start, w, h, round, shift, gain_limit, accum);
        vif_write_downsample(buf, x_start, y, w, h, sum);
    }
}

#define FILTER1D_8_VERT(alignment_type, fwidth_0, fwidth_1)                                        \
    __global__ void filter1d_8_vertical_kernel_##alignment_type##_##fwidth_0##_##fwidth_1(         \
        VifBufferCuda buf, uint8_t *ref_in, uint8_t *dis_in, int w, int h,                         \
        filter_table_stuct vif_filt_s0)                                                            \
    {                                                                                              \
        filter1d_8_vertical_kernel<alignment_type, fwidth_0, fwidth_1>(buf, ref_in, dis_in, w, h,  \
                                                                       vif_filt_s0);               \
    }

/*
 * __launch_bounds__(128, 10) hint: BLOCKX=128 threads, min 10 blocks/SM.
 * On sm_89 (RTX 4090, 65536 regs/SM): floor(65536/128/10)=51-reg budget ->
 * ptxas allocates 48 regs (down from 56 at baseline).  Theoretical occupancy
 * rises from 75% to 83.3% (ADR-0743 perf-audit win #2).
 * On sm_75/sm_80/sm_86 (1024 max threads/SM): 10x128=1280 > 1024, so ptxas
 * emits a "minnctapersm out of range, ignored" advisory for those targets --
 * these targets keep 56 registers (no regression, just no gain).
 */
#define FILTER1D_8_HORI(val_per_thread, fwidth_0, fwidth_1)                                        \
    __global__ __launch_bounds__(128, 10) void                                                     \
    filter1d_8_horizontal_kernel_##val_per_thread##_##fwidth_0##_##fwidth_1(                       \
        VifBufferCuda buf, int w, int h, filter_table_stuct vif_filt_s0,                           \
        double vif_enhn_gain_limit, vif_accums *accum)                                             \
    {                                                                                              \
        filter1d_8_horizontal_kernel<val_per_thread, fwidth_0, fwidth_1>(                          \
            buf, w, h, vif_filt_s0, vif_enhn_gain_limit, accum);                                   \
    }

#define FILTER1D_16_VERT(alignment_type, fwidth, fwidth_rd, scale)                                 \
    __global__ void                                                                                \
    filter1d_16_vertical_kernel_##alignment_type##_##fwidth##_##fwidth_rd##_##scale(               \
        VifBufferCuda buf, uint16_t *ref_in, uint16_t *dis_in, int w, int h,                       \
        int32_t add_shift_round_VP, int32_t shift_VP, int32_t add_shift_round_VP_sq,               \
        int32_t shift_VP_sq, filter_table_stuct vif_filt)                                          \
    {                                                                                              \
        filter1d_16_vertical_kernel<alignment_type, fwidth, fwidth_rd, scale>(                     \
            buf, ref_in, dis_in, w, h, add_shift_round_VP, shift_VP, add_shift_round_VP_sq,        \
            shift_VP_sq, vif_filt);                                                                \
    }

#define FILTER1D_16_HORI(val_per_thread, fwidth, fwidth_rd, scale)                                 \
    __global__ void                                                                                \
    filter1d_16_horizontal_kernel_##val_per_thread##_##fwidth##_##fwidth_rd##_##scale(             \
        VifBufferCuda buf, int w, int h, int32_t add_shift_round_HP, int32_t shift_HP,             \
        filter_table_stuct vif_filt, double vif_enhn_gain_limit, vif_accums *accum)                \
    {                                                                                              \
        filter1d_16_horizontal_kernel<val_per_thread, fwidth, fwidth_rd, scale>(                   \
            buf, w, h, add_shift_round_HP, shift_HP, vif_filt, vif_enhn_gain_limit, accum);        \
    }

extern "C" {
// constexpr int fwidth[4] = {17, 9, 5, 3};
FILTER1D_8_VERT(uint32_t, 17, 9); // filter1d_8_vertical_kernel_uint32_t_17_9
/*
 * val_per_thread=4 was evaluated (ADR-0743): smem grows 7644->14812 B/block,
 * making the 17-tap kernel smem-limited at 37.5% occupancy on sm_89 vs 83.3%
 * for vpt=2 with __launch_bounds__.  vpt=4 reverted.
 */
FILTER1D_8_HORI(2, 17, 9);         // filter1d_8_horizontal_kernel_2_17_9
FILTER1D_16_VERT(uint2, 17, 9, 0); // filter1d_16_vertical_kernel_uint2_17_9_0
FILTER1D_16_VERT(uint2, 9, 5, 1);  // filter1d_16_vertical_kernel_uint2_9_5_1
FILTER1D_16_VERT(uint2, 5, 3, 2);  // filter1d_16_vertical_kernel_uint2_5_3_2
FILTER1D_16_VERT(uint2, 3, 0, 3);  // filter1d_16_vertical_kernel_uint2_3_0_3

FILTER1D_16_HORI(2, 17, 9, 0); // filter1d_16_horizontal_kernel_2_17_9_0
FILTER1D_16_HORI(2, 9, 5, 1);  // filter1d_16_horizontal_kernel_2_9_5_1
FILTER1D_16_HORI(2, 5, 3, 2);  // filter1d_16_horizontal_kernel_2_5_3_2
FILTER1D_16_HORI(2, 3, 0, 3);  // filter1d_16_horizontal_kernel_2_3_0_3
}
