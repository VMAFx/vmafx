/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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

#include "cuda_device_ptr.cuh"
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
 * The kernels below are assembled from short stages (load a tile into shared
 * memory, accumulate the taps, round, write back).  Every stage is
 * __forceinline__ and integer-only, so a kernel is the same sequence of
 * operations it was as one function body.
 * ------------------------------------------------------------------------- */

/* Static shared-memory bounds of the vertical tiles: BLOCKY_MAX=8 covers any
 * valid launch (the actual BLOCKY is 4), and 128 columns are BLOCKX(=32) *
 * val_per_thread(=4). */
#define VIF_VERT_BLOCKY_MAX 8
#define VIF_VERT_TILE_COLS 128

/* The 16 bytes a thread writes back at once: four uint32 sums. */
#define VIF_WRITEBACK_BYTES sizeof(uint4)
#define VIF_WRITEBACK_SUMS (sizeof(uint4) / sizeof(uint32_t))

namespace
{

/* Mirror `i` into [0, n): reflect at 0, then at n - 1 (two bounces), then
 * clamp, which only acts on an axis shorter than the filter.  Applied while
 * a tile is loaded, so the compute stages read shared memory without any
 * boundary check. */
__device__ __forceinline__ int vif_mirror_index(int i, int n)
{
    if (i < 0) {
        i = -i;
    }
    if (i >= n) {
        i = 2 * n - i - 2;
    }
    if (i < 0) {
        i = 0;
    }
    if (i >= n) {
        i = n - 1;
    }
    return i;
}

/* Cooperative load of the vertical tiles.  Thread (ty, tx) owns tile columns
 * [tx*vpt, tx*vpt + vpt) and iterates over tile rows stepping by blockDim.y.
 * tile_row=0 maps to image row (blockIdx.y*blockDim.y - fwidth/2).
 * `stride` / `dis_stride` are the pitches of `ref_in` / `dis_in` in samples:
 * two pictures may have different pitches (ADR-2023). */
template <typename alignment_type, typename sample_type, int fwidth>
__device__ __forceinline__ void
vif_vert_load_tiles(sample_type (*ref_tile)[VIF_VERT_TILE_COLS],
                    sample_type (*dis_tile)[VIF_VERT_TILE_COLS], const sample_type *ref_in,
                    const sample_type *dis_in, ptrdiff_t stride, ptrdiff_t dis_stride, int w, int h)
{
    constexpr int val_per_thread = sizeof(alignment_type) / sizeof(sample_type);
    constexpr int half_fv = fwidth / 2;
    const int x_block_start = blockIdx.x * blockDim.x * val_per_thread;
    const int tile_h = blockDim.y + fwidth - 1;
    const int col = threadIdx.x * val_per_thread;

    for (int tile_row = threadIdx.y; tile_row < tile_h; tile_row += blockDim.y) {
        const int img_row =
            vif_mirror_index((int)(blockIdx.y * blockDim.y) - half_fv + tile_row, h);
        if (x_block_start + col < w) {
            const alignment_type ref_vec = *reinterpret_cast<const alignment_type *>(
                &ref_in[(ptrdiff_t)img_row * stride + x_block_start + col]);
            const alignment_type dis_vec = *reinterpret_cast<const alignment_type *>(
                &dis_in[(ptrdiff_t)img_row * dis_stride + x_block_start + col]);
            const sample_type *ref_s = reinterpret_cast<const sample_type *>(&ref_vec);
            const sample_type *dis_s = reinterpret_cast<const sample_type *>(&dis_vec);
#pragma unroll
            for (int k = 0; k < val_per_thread; ++k) {
                ref_tile[tile_row][col + k] = ref_s[k];
                dis_tile[tile_row][col + k] = dis_s[k];
            }
        }
    }
}

/* Four sums of one tmp plane, written as one uint4. */
__device__ __forceinline__ void vif_store_sums(uint32_t *plane, int buffer_idx,
                                               const uint32_t *sums)
{
    *reinterpret_cast<uint4 *>(&plane[buffer_idx]) = *reinterpret_cast<const uint4 *>(sums);
}

/* The rounded sums of a vertical pass, as the write-back takes them.  Every
 * array is aligned for the uint4 store. */
template <int vpt> struct VifVertOut {
    __align__(VIF_WRITEBACK_BYTES) uint32_t mu1[vpt];
    __align__(VIF_WRITEBACK_BYTES) uint32_t mu2[vpt];
    __align__(VIF_WRITEBACK_BYTES) uint32_t ref[vpt];
    __align__(VIF_WRITEBACK_BYTES) uint32_t dis[vpt];
    __align__(VIF_WRITEBACK_BYTES) uint32_t ref_dis[vpt];
    __align__(VIF_WRITEBACK_BYTES) uint32_t ref_rd[vpt];
    __align__(VIF_WRITEBACK_BYTES) uint32_t dis_rd[vpt];
};

/* Write-back of a vertical pass to the seven tmp planes (five when the scale
 * has no reduced filter). */
template <int vpt, bool has_rd>
__device__ __forceinline__ void vif_vert_store(const VifBufferCuda &buf, VifVertOut<vpt> &o, int y,
                                               int x_start, int w)
{
    const int stride_tmp = buf.stride_tmp / sizeof(uint32_t);
    for (int idx = 0; idx < vpt; idx += VIF_WRITEBACK_SUMS) {
        const int buffer_idx = y * stride_tmp + x_start + idx;
        if (x_start + idx < w) {
            vif_store_sums(buf.tmp.mu1, buffer_idx, &o.mu1[idx]);
            vif_store_sums(buf.tmp.mu2, buffer_idx, &o.mu2[idx]);
            vif_store_sums(buf.tmp.ref, buffer_idx, &o.ref[idx]);
            vif_store_sums(buf.tmp.dis, buffer_idx, &o.dis[idx]);
            vif_store_sums(buf.tmp.ref_dis, buffer_idx, &o.ref_dis[idx]);
            if (has_rd) {
                vif_store_sums(buf.tmp.ref_convol, buffer_idx, &o.ref_rd[idx]);
                vif_store_sums(buf.tmp.dis_convol, buffer_idx, &o.dis_rd[idx]);
            }
        }
    }
}

/* One 8-bit vertical tap `fi` of output sample `off`: the means, the second
 * moments and, inside the reduced filter's window, its two sums. */
template <int vpt, int fwidth_0, int fwidth_1>
__device__ __forceinline__ void vif_vert8_tap(VifVertOut<vpt> &o, int off, int fi, uint32_t ref_val,
                                              uint32_t dis_val,
                                              const filter_table_stuct &vif_filt_s0)
{
    const uint32_t fcoeff = vif_filt_s0.filter[0][fi];
    const uint32_t img_coeff_ref = fcoeff * ref_val;
    const uint32_t img_coeff_dis = fcoeff * dis_val;
    o.mu1[off] += img_coeff_ref;
    o.mu2[off] += img_coeff_dis;
    o.ref[off] += img_coeff_ref * ref_val;
    o.dis[off] += img_coeff_dis * dis_val;
    o.ref_dis[off] += img_coeff_ref * dis_val;
    if (fi >= (fwidth_0 - fwidth_1) / 2 && fi < (fwidth_0 - (fwidth_0 - fwidth_1) / 2)) {
        const uint16_t fcoeff_rd = vif_filt_s0.filter[1][fi - ((fwidth_0 - fwidth_1) / 2)];
        o.ref_rd[off] += fcoeff_rd * ref_val;
        o.dis_rd[off] += fcoeff_rd * dis_val;
    }
}

/* 8-bit vertical taps.  smem_row = threadIdx.y + fi: tile_row=0 holds image
 * row (blockIdx.y*blockDim.y - half_fv), so the thread at threadIdx.y finds
 * tap fi's mirrored row at tile row threadIdx.y + fi. */
template <int vpt, int fwidth_0, int fwidth_1>
__device__ __forceinline__ void
vif_vert8_accumulate(VifVertOut<vpt> &o, uint8_t (*ref_tile)[VIF_VERT_TILE_COLS],
                     uint8_t (*dis_tile)[VIF_VERT_TILE_COLS], int col, int x_start, int w,
                     const filter_table_stuct &vif_filt_s0)
{
    for (int fi = 0; fi < fwidth_0; ++fi) {
        const int smem_row = threadIdx.y + fi;
        for (int off = 0; off < vpt; ++off) {
            const int j = x_start + off;
            if (j < w) {
                const uint32_t ref_val = ref_tile[smem_row][col + off];
                const uint32_t dis_val = dis_tile[smem_row][col + off];
                vif_vert8_tap<vpt, fwidth_0, fwidth_1>(o, off, fi, ref_val, dis_val, vif_filt_s0);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * 8-bit VERTICAL KERNEL (win #4)
 * Stages ref_in / dis_in rows into shared memory before the accumulation loop.
 * Block size: BLOCKX=32, BLOCKY=4, val_per_thread=4 (uint32_t alignment).
 * Tile: (BLOCKY + fwidth_0 - 1) rows × (BLOCKX * val_per_thread = 128) cols.
 * For fwidth_0=17: (4+16)=20 rows × 128 cols × 2 planes × 1 B = 5120 B.
 * The static smem size uses BLOCKY_MAX=8: 2 * 24 * 128 = 6144 bytes.
 * ------------------------------------------------------------------------- */
template <typename alignment_type = uint2, int fwidth_0 = 17, int fwidth_1 = 9>
__device__ __forceinline__ void filter1d_8_vertical_kernel(VifBufferCuda buf, uint8_t *ref_in,
                                                           uint8_t *dis_in, int w, int h,
                                                           filter_table_stuct vif_filt_s0)
{
    constexpr int val_per_thread = sizeof(alignment_type);
    static_assert(val_per_thread % 4 == 0 && val_per_thread <= 16,
                  "val per thread bust be divisible by 4 and under 16");

    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const int x_start = (blockIdx.x * blockDim.x + threadIdx.x) * val_per_thread;

    constexpr int TILE_H_MAX = VIF_VERT_BLOCKY_MAX + fwidth_0 - 1;
    __shared__ uint8_t ref_tile[TILE_H_MAX][VIF_VERT_TILE_COLS];
    __shared__ uint8_t dis_tile[TILE_H_MAX][VIF_VERT_TILE_COLS];

    vif_vert_load_tiles<alignment_type, uint8_t, fwidth_0>(ref_tile, dis_tile, ref_in, dis_in,
                                                           buf.stride, buf.dis_stride, w, h);
    __syncthreads();

    if (x_start < w && y < h) {
        VifVertOut<val_per_thread> o = {};
        vif_vert8_accumulate<val_per_thread, fwidth_0, fwidth_1>(
            o, ref_tile, dis_tile, threadIdx.x * val_per_thread, x_start, w, vif_filt_s0);
        for (int off = 0; off < val_per_thread; ++off) {
            o.mu1[off] = (o.mu1[off] + 128) >> 8u;
            o.mu2[off] = (o.mu2[off] + 128) >> 8u;
            o.ref_rd[off] = (o.ref_rd[off] + 128) >> 8u;
            o.dis_rd[off] = (o.dis_rd[off] + 128) >> 8u;
        }
        vif_vert_store<val_per_thread, true>(buf, o, y, x_start, w);
    }
}

/* The seven tmp channels of one row tile in shared memory. */
struct VifHoriTile {
    uint32_t *mu1;
    uint32_t *mu2;
    uint32_t *ref;
    uint32_t *dis;
    uint32_t *ref_dis;
    uint32_t *ref_convol;
    uint32_t *dis_convol;
};

/* One tmp sample.  With `use_ldg` the load goes through the read-only /
 * texture L1 cache: the 7 tmp channels are written only by the preceding
 * vertical pass, and at >=1080p they exceed L2 capacity per frame, so L1-RO
 * spreads the hot-slice pressure (ADR-0743 win #3). */
template <bool use_ldg> __device__ __forceinline__ uint32_t vif_tmp_load(const uint32_t *p)
{
    return use_ldg ? __ldg(p) : *p;
}

/* Cooperative load of a row tile: tile_w - 1 positions (the last element is
 * padding, never read).  Each thread loads elements spaced blockDim.x apart
 * starting at si=threadIdx.x.  Mirror boundary applied here. */
template <int tile_w, bool use_ldg>
__device__ __forceinline__ void vif_hori_load_tile(const VifHoriTile &t, const VifBufferCuda &buf,
                                                   int buf_row, int tile_x0, int w)
{
    for (int si = threadIdx.x; si < tile_w - 1; si += blockDim.x) {
        const int img_col = vif_mirror_index(tile_x0 + si, w);
        t.mu1[si] = vif_tmp_load<use_ldg>(&buf.tmp.mu1[buf_row + img_col]);
        t.mu2[si] = vif_tmp_load<use_ldg>(&buf.tmp.mu2[buf_row + img_col]);
        t.ref[si] = vif_tmp_load<use_ldg>(&buf.tmp.ref[buf_row + img_col]);
        t.dis[si] = vif_tmp_load<use_ldg>(&buf.tmp.dis[buf_row + img_col]);
        t.ref_dis[si] = vif_tmp_load<use_ldg>(&buf.tmp.ref_dis[buf_row + img_col]);
        t.ref_convol[si] = vif_tmp_load<use_ldg>(&buf.tmp.ref_convol[buf_row + img_col]);
        t.dis_convol[si] = vif_tmp_load<use_ldg>(&buf.tmp.dis_convol[buf_row + img_col]);
    }
}

/* The running sums of a horizontal pass for a thread's val_per_thread output
 * pixels; the reduced filter produces one value per two pixels. */
template <int vpt> struct VifHoriSums {
    uint32_t mu1[vpt];
    uint32_t mu2[vpt];
    uint64_t ref_tmp[vpt];
    uint64_t dis_tmp[vpt];
    uint64_t ref_dis_tmp[vpt];
    uint32_t ref_rd[vpt / 2];
    uint32_t dis_rd[vpt / 2];
};

/* Interior path, centre tap (unpaired).  smem[smem_base + off + fj] holds the
 * value at image column (x_start + off - half_fw + fj) after mirror-clamping. */
template <int vpt, int fwidth, int fwidth_rd>
__device__ __forceinline__ void vif_hori_center_tap(VifHoriSums<vpt> &s, const VifHoriTile &t,
                                                    int smem_base, const uint16_t *filt,
                                                    const uint16_t *filt_rd)
{
    constexpr int half_fw = fwidth / 2;
    const uint16_t fcoeff = filt[half_fw];
#pragma unroll
    for (int off = 0; off < vpt; ++off) {
        const int si = smem_base + off + half_fw;
        s.mu1[off] += fcoeff * t.mu1[si];
        s.mu2[off] += fcoeff * t.mu2[si];
        s.ref_tmp[off] += fcoeff * (uint64_t)t.ref[si];
        s.dis_tmp[off] += fcoeff * (uint64_t)t.dis[si];
        s.ref_dis_tmp[off] += fcoeff * (uint64_t)t.ref_dis[si];
    }
    if (fwidth_rd > 0) {
        const uint32_t fcoeff_rd = filt_rd[fwidth_rd / 2];
#pragma unroll
        for (int off = 0; off < vpt; off += 2) {
            const int si = smem_base + off + half_fw;
            s.ref_rd[off / 2] += fcoeff_rd * t.ref_convol[si];
            s.dis_rd[off / 2] += fcoeff_rd * t.dis_convol[si];
        }
    }
}

/* Interior path, symmetric tap pairs: half the multiplies of a tap-by-tap
 * loop (17 -> 9 at scale 0). */
template <int vpt, int fwidth, int fwidth_rd>
__device__ __forceinline__ void vif_hori_tap_pairs(VifHoriSums<vpt> &s, const VifHoriTile &t,
                                                   int smem_base, const uint16_t *filt,
                                                   const uint16_t *filt_rd)
{
    constexpr int half_fw = fwidth / 2;
    constexpr int rd_start = (fwidth - fwidth_rd) / 2;
    constexpr int rd_half = fwidth_rd / 2;
#pragma unroll
    for (int fj = 0; fj < half_fw; ++fj) {
        const uint16_t fcoeff = filt[fj];
#pragma unroll
        for (int off = 0; off < vpt; ++off) {
            const int si_lo = smem_base + off + fj;
            const int si_hi = smem_base + off + 2 * half_fw - fj;
            s.mu1[off] += fcoeff * (t.mu1[si_lo] + t.mu1[si_hi]);
            s.mu2[off] += fcoeff * (t.mu2[si_lo] + t.mu2[si_hi]);
            s.ref_tmp[off] += fcoeff * ((uint64_t)t.ref[si_lo] + (uint64_t)t.ref[si_hi]);
            s.dis_tmp[off] += fcoeff * ((uint64_t)t.dis[si_lo] + (uint64_t)t.dis[si_hi]);
            s.ref_dis_tmp[off] +=
                fcoeff * ((uint64_t)t.ref_dis[si_lo] + (uint64_t)t.ref_dis[si_hi]);
        }
        if (fwidth_rd > 0 && fj >= rd_start && fj < rd_start + rd_half) {
            const uint32_t fcoeff_rd = filt_rd[fj - rd_start];
#pragma unroll
            for (int off = 0; off < vpt; off += 2) {
                const int si_lo = smem_base + off + fj;
                const int si_hi = smem_base + off + 2 * half_fw - fj;
                s.ref_rd[off / 2] += fcoeff_rd * (t.ref_convol[si_lo] + t.ref_convol[si_hi]);
                s.dis_rd[off / 2] += fcoeff_rd * (t.dis_convol[si_lo] + t.dis_convol[si_hi]);
            }
        }
    }
}

/* One border tap `fj` of output sample `off`; `si` is its shared-memory index. */
template <int vpt, int fwidth, int fwidth_rd>
__device__ __forceinline__ void vif_hori_border_tap(VifHoriSums<vpt> &s, const VifHoriTile &t,
                                                    int off, int fj, int si, const uint16_t *filt,
                                                    const uint16_t *filt_rd)
{
    constexpr int rd_start = (fwidth - fwidth_rd) / 2;
    const uint16_t fcoeff = filt[fj];
    s.mu1[off] += fcoeff * t.mu1[si];
    s.mu2[off] += fcoeff * t.mu2[si];
    s.ref_tmp[off] += fcoeff * (uint64_t)t.ref[si];
    s.dis_tmp[off] += fcoeff * (uint64_t)t.dis[si];
    s.ref_dis_tmp[off] += fcoeff * (uint64_t)t.ref_dis[si];

    if (fj >= rd_start && fj < (fwidth - rd_start) && fwidth_rd > 0 && off % 2 == 0) {
        const uint32_t fcoeff_rd = filt_rd[fj - rd_start];
        s.ref_rd[off / 2] += fcoeff_rd * t.ref_convol[si];
        s.dis_rd[off / 2] += fcoeff_rd * t.dis_convol[si];
    }
}

/* Border path: the tile already holds the mirrored boundary values, so
 * si = smem_base + off + fj is the same formula as in the interior.  The
 * per-element `j < w` guard stays because x_start+off may exceed w at the
 * right edge of the last block. */
template <int vpt, int fwidth, int fwidth_rd>
__device__ __forceinline__ void vif_hori_border(VifHoriSums<vpt> &s, const VifHoriTile &t,
                                                int smem_base, int x_start, int w,
                                                const uint16_t *filt, const uint16_t *filt_rd)
{
#pragma unroll
    for (int fj = 0; fj < fwidth; ++fj) {
#pragma unroll
        for (int off = 0; off < vpt; ++off) {
            const int j = x_start + off;
            if (j < w) {
                vif_hori_border_tap<vpt, fwidth, fwidth_rd>(s, t, off, fj, smem_base + off + fj,
                                                            filt, filt_rd);
            }
        }
    }
}

/* Round the second-moment sums and add each pixel's VIF statistics to the
 * thread's accumulators. */
template <int vpt>
__device__ __forceinline__ void
vif_hori_statistics(const VifHoriSums<vpt> &s, int x_start, int w, int32_t add_shift_round_HP,
                    int32_t shift_HP, double vif_enhn_gain_limit, vif_accums &thread_accum)
{
    const uint32_t shift = (uint32_t)shift_HP;
    for (int off = 0; off < vpt; ++off) {
        const int x = x_start + off;
        if (x < w) {
            const uint32_t accum_ref = (uint32_t)((s.ref_tmp[off] + add_shift_round_HP) >> shift);
            const uint32_t accum_dis = (uint32_t)((s.dis_tmp[off] + add_shift_round_HP) >> shift);
            const uint32_t accum_ref_dis =
                (uint32_t)((s.ref_dis_tmp[off] + add_shift_round_HP) >> shift);
            vif_statistic_calculation<uint32_t>(s.mu1[off], s.mu2[off], accum_ref, accum_dis,
                                                accum_ref_dis, x, w, vif_enhn_gain_limit,
                                                thread_accum);
        }
    }
}

/* Warp-reduce the thread's seven accumulators and add each warp's sums to the
 * frame accumulators. Every lane of the warp calls it, a lane past the plane
 * edge with zeros: warp_reduce() shuffles with the full mask
 * (T-CUDA-WARP-REDUCE-UB-2026-10-05). */
__device__ __forceinline__ void vif_hori_flush_accums(int64_t *thread_accum_i64, vif_accums *accum)
{
    for (int i = 0; i < 7; ++i) {
        thread_accum_i64[i] = warp_reduce(thread_accum_i64[i]);
    }
    const int warp_id = threadIdx.x % VMAF_CUDA_THREADS_PER_WARP;
    if (warp_id == 0) {
        for (int i = 0; i < 7; ++i) {
            atomicAdd_int64(&reinterpret_cast<int64_t *>(accum)[i], thread_accum_i64[i]);
        }
    }
}

/* The reduced-filter outputs: the next scale's ref / dis, on even rows and
 * even columns. */
template <int vpt>
__device__ __forceinline__ void vif_hori_store_rd(const VifBufferCuda &buf,
                                                  const VifHoriSums<vpt> &s, int y, int x_start,
                                                  int w, int h)
{
    uint16_t *ref = VMAF_CUDA_DPTR(uint16_t, buf.ref);
    uint16_t *dis = VMAF_CUDA_DPTR(uint16_t, buf.dis);
    for (int off = 0; off < vpt; ++off) {
        const int x = x_start + off;
        if (y < h && x < w) {
            if ((y % 2) == 0 && (off % 2) == 0) {
                const ptrdiff_t rd_stride = buf.rd_stride / sizeof(uint16_t);
                ref[(y / 2) * rd_stride + (x / 2)] = (uint16_t)((s.ref_rd[off / 2] + 32768) >> 16u);
                dis[(y / 2) * rd_stride + (x / 2)] = (uint16_t)((s.dis_rd[off / 2] + 32768) >> 16u);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * HORIZONTAL KERNEL, both bit depths (win #1 + ncu-driven perf ADR-0743)
 * Stages all 7 tmp channels into shared memory before the filter loop.
 * Block size: BLOCKX=128, BLOCKY=1, val_per_thread=2.
 * Tile: HORI_TILE_W elements per channel × 7 channels × 4 B.
 * For fwidth=17 (half_fw=8): (256+16+1)=273 × 7 × 4 = 7644 B per block.
 * TILE_W = BLOCKX*vpt + 2*half_fw + 1 (padding); the tile starts at image
 * column tile_x0 = blockIdx.x * blockDim.x * val_per_thread - half_fw.
 * `filt_row` is the scale's row of the filter table; the reduced filter is
 * the next row.  The interior fast path needs no boundary check: all taps
 * land within [0, w-1] after the load stage.
 * ------------------------------------------------------------------------- */
template <int val_per_thread, int fwidth, int fwidth_rd, int filt_row, bool use_ldg>
__device__ __forceinline__ void
vif_hori_kernel(VifBufferCuda buf, int w, int h, int32_t add_shift_round_HP, int32_t shift_HP,
                filter_table_stuct vif_filt, double vif_enhn_gain_limit, vif_accums *accum)
{
    static_assert(val_per_thread % 2 == 0, "val_per_thread must be divisible by 2");

    constexpr int half_fw = fwidth / 2;
    constexpr int TILE_W = HORI_TILE_W(128, val_per_thread, half_fw);

    __shared__ uint32_t smem_mu1[TILE_W];
    __shared__ uint32_t smem_mu2[TILE_W];
    __shared__ uint32_t smem_ref[TILE_W];
    __shared__ uint32_t smem_dis[TILE_W];
    __shared__ uint32_t smem_ref_dis[TILE_W];
    __shared__ uint32_t smem_ref_convol[TILE_W];
    __shared__ uint32_t smem_dis_convol[TILE_W];
    const VifHoriTile tile = {smem_mu1,     smem_mu2,        smem_ref,       smem_dis,
                              smem_ref_dis, smem_ref_convol, smem_dis_convol};

    const int y = blockIdx.y;
    const int x_start = (blockIdx.x * blockDim.x + threadIdx.x) * val_per_thread;
    const int tile_x0 = (int)(blockIdx.x * blockDim.x) * val_per_thread - half_fw;
    const int stride_tmp = buf.stride_tmp / sizeof(uint32_t);
    const int buf_row = y * stride_tmp;

    if (y < h) {
        vif_hori_load_tile<TILE_W, use_ldg>(tile, buf, buf_row, tile_x0, w);
    }
    __syncthreads();

    union {
        vif_accums thread_accum;
        int64_t thread_accum_i64[7] = {0};
    };
    if (y < h && x_start < w) {
        VifHoriSums<val_per_thread> sums = {};
        const uint16_t *filt = vif_filt.filter[filt_row];
        const uint16_t *filt_rd = vif_filt.filter[(fwidth_rd > 0) ? filt_row + 1 : filt_row];
        const int smem_base = threadIdx.x * val_per_thread;

        const bool interior = (x_start >= half_fw) && (x_start + val_per_thread - 1 + half_fw < w);
        if (interior) {
            vif_hori_center_tap<val_per_thread, fwidth, fwidth_rd>(sums, tile, smem_base, filt,
                                                                   filt_rd);
            vif_hori_tap_pairs<val_per_thread, fwidth, fwidth_rd>(sums, tile, smem_base, filt,
                                                                  filt_rd);
        } else {
            vif_hori_border<val_per_thread, fwidth, fwidth_rd>(sums, tile, smem_base, x_start, w,
                                                               filt, filt_rd);
        }
        vif_hori_statistics<val_per_thread>(sums, x_start, w, add_shift_round_HP, shift_HP,
                                            vif_enhn_gain_limit, thread_accum);
        vif_hori_store_rd<val_per_thread>(buf, sums, y, x_start, w, h);
    }
    /* y is the same for the whole block; the lanes past the plane edge add
     * zeros, so every lane of a warp reaches the full-mask shuffles. */
    if (y < h) {
        vif_hori_flush_accums(thread_accum_i64, accum);
    }
}

/* 8-bit horizontal kernel: scale 0, a fixed rounding of 2^15 and a shift of
 * 16, tmp loads through __ldg().  __launch_bounds__(128, 10) on the kernel
 * entry caps registers 56→48 on sm_89 (RTX 4090), lifting theoretical
 * occupancy 75%→83.3% (ADR-0743 perf-audit win #2). */
template <int val_per_thread = 1, int fwidth_0 = 17, int fwidth_1 = 9>
__device__ __forceinline__ void
filter1d_8_horizontal_kernel(VifBufferCuda buf, int w, int h, filter_table_stuct vif_filt_s0,
                             double vif_enhn_gain_limit, vif_accums *accum)
{
    vif_hori_kernel<val_per_thread, fwidth_0, fwidth_1, 0, true>(buf, w, h, 32768, 16, vif_filt_s0,
                                                                 vif_enhn_gain_limit, accum);
}

/* The sums of a 16-bit vertical pass before rounding. */
template <int vpt> struct VifVert16Sums {
    uint64_t ref[vpt];
    uint64_t dis[vpt];
    uint64_t ref_dis[vpt];
};

/* One 16-bit vertical tap `fi` of output sample `off`: the means and the
 * reduced filter in 32 bits (in `o`), the second moments in 64 bits (in `s`). */
template <int vpt, int fwidth, int fwidth_rd, int scale>
__device__ __forceinline__ void vif_vert16_tap(VifVertOut<vpt> &o, VifVert16Sums<vpt> &s, int off,
                                               int fi, uint32_t imgcoeff_ref, uint32_t imgcoeff_dis,
                                               const filter_table_stuct &vif_filt)
{
    const uint16_t fcoeff = vif_filt.filter[scale][fi];
    const uint32_t img_coeff_ref = fcoeff * imgcoeff_ref;
    const uint32_t img_coeff_dis = fcoeff * imgcoeff_dis;
    o.mu1[off] += img_coeff_ref;
    o.mu2[off] += img_coeff_dis;
    s.ref[off] += img_coeff_ref * (uint64_t)imgcoeff_ref;
    s.dis[off] += img_coeff_dis * (uint64_t)imgcoeff_dis;
    s.ref_dis[off] += img_coeff_ref * (uint64_t)imgcoeff_dis;
    if (fi >= (fwidth - fwidth_rd) / 2 && fi < (fwidth - (fwidth - fwidth_rd) / 2) &&
        fwidth_rd > 0) {
        const uint16_t fcoeff_rd = vif_filt.filter[scale + 1][fi - ((fwidth - fwidth_rd) / 2)];
        o.ref_rd[off] += fcoeff_rd * imgcoeff_ref;
        o.dis_rd[off] += fcoeff_rd * imgcoeff_dis;
    }
}

/* 16-bit vertical taps over the tile rows. */
template <int vpt, int fwidth, int fwidth_rd, int scale>
__device__ __forceinline__ void vif_vert16_accumulate(VifVertOut<vpt> &o, VifVert16Sums<vpt> &s,
                                                      uint16_t (*ref_tile)[VIF_VERT_TILE_COLS],
                                                      uint16_t (*dis_tile)[VIF_VERT_TILE_COLS],
                                                      int col, int x_start, int w,
                                                      const filter_table_stuct &vif_filt)
{
    for (int fi = 0; fi < fwidth; ++fi) {
        const int smem_row = threadIdx.y + fi;
        for (int off = 0; off < vpt; ++off) {
            const int j = x_start + off;
            if (j < w) {
                const uint32_t imgcoeff_ref = ref_tile[smem_row][col + off];
                const uint32_t imgcoeff_dis = dis_tile[smem_row][col + off];
                vif_vert16_tap<vpt, fwidth, fwidth_rd, scale>(o, s, off, fi, imgcoeff_ref,
                                                              imgcoeff_dis, vif_filt);
            }
        }
    }
}

/* Rounding of a 16-bit vertical pass with the scale's shifts. */
template <int vpt, bool has_rd>
__device__ __forceinline__ void vif_vert16_round(VifVertOut<vpt> &o, const VifVert16Sums<vpt> &s,
                                                 int32_t add_shift_round_VP, int32_t shift_VP,
                                                 int32_t add_shift_round_VP_sq, int32_t shift_VP_sq)
{
    const uint32_t sh = (uint32_t)shift_VP;
    const uint32_t sh_sq = (uint32_t)shift_VP_sq;
    for (int off = 0; off < vpt; ++off) {
        o.mu1[off] = (uint16_t)((o.mu1[off] + add_shift_round_VP) >> sh);
        o.mu2[off] = (uint16_t)((o.mu2[off] + add_shift_round_VP) >> sh);
        o.ref[off] = (uint32_t)((s.ref[off] + add_shift_round_VP_sq) >> sh_sq);
        o.dis[off] = (uint32_t)((s.dis[off] + add_shift_round_VP_sq) >> sh_sq);
        o.ref_dis[off] = (uint32_t)((s.ref_dis[off] + add_shift_round_VP_sq) >> sh_sq);
        if (has_rd) {
            o.ref_rd[off] = (uint16_t)((o.ref_rd[off] + add_shift_round_VP) >> sh);
            o.dis_rd[off] = (uint16_t)((o.dis_rd[off] + add_shift_round_VP) >> sh);
        }
    }
}

/* -------------------------------------------------------------------------
 * 16-bit VERTICAL KERNEL (win #4, 16-bit variant)
 * Block size: BLOCK_VERT_X=32, BLOCK_VERT_Y=4, val_per_thread=4 (uint16_t).
 * Tile: (BLOCK_VERT_Y + fwidth - 1) rows × 128 cols × 2 planes × 2 B.
 * For fwidth=17: (4+16)=20 rows × 128 cols × 2 × 2 = 10240 B per block; the
 * static smem size uses BLOCKY_MAX=8: 2 * 24 * 128 * 2 = 12288 bytes.
 * ------------------------------------------------------------------------- */
template <typename alignment_type = uint2, int fwidth, int fwidth_rd, int scale>
__device__ __forceinline__ void
filter1d_16_vertical_kernel(VifBufferCuda buf, uint16_t *ref_in, uint16_t *dis_in, int w, int h,
                            int32_t add_shift_round_VP, int32_t shift_VP,
                            int32_t add_shift_round_VP_sq, int32_t shift_VP_sq,
                            filter_table_stuct vif_filt)
{
    constexpr int val_per_thread = sizeof(alignment_type) / sizeof(uint16_t);
    static_assert(val_per_thread % 4 == 0 && val_per_thread <= 8,
                  "val per thread bust be divisible by 4 and under 16");

    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const int x_start = (blockIdx.x * blockDim.x + threadIdx.x) * val_per_thread;

    constexpr int TILE_H_MAX = VIF_VERT_BLOCKY_MAX + fwidth - 1;
    __shared__ uint16_t ref_tile16[TILE_H_MAX][VIF_VERT_TILE_COLS];
    __shared__ uint16_t dis_tile16[TILE_H_MAX][VIF_VERT_TILE_COLS];

    const ptrdiff_t stride =
        (scale == 0) ? buf.stride / sizeof(uint16_t) : buf.rd_stride / sizeof(uint16_t);
    const ptrdiff_t dis_stride =
        (scale == 0) ? buf.dis_stride / sizeof(uint16_t) : buf.rd_stride / sizeof(uint16_t);
    vif_vert_load_tiles<alignment_type, uint16_t, fwidth>(ref_tile16, dis_tile16, ref_in, dis_in,
                                                          stride, dis_stride, w, h);
    __syncthreads();

    if (x_start < w && y < h) {
        VifVertOut<val_per_thread> o = {};
        VifVert16Sums<val_per_thread> s = {};
        vif_vert16_accumulate<val_per_thread, fwidth, fwidth_rd, scale>(
            o, s, ref_tile16, dis_tile16, threadIdx.x * val_per_thread, x_start, w, vif_filt);
        vif_vert16_round<val_per_thread, (fwidth_rd > 0)>(o, s, add_shift_round_VP, shift_VP,
                                                          add_shift_round_VP_sq, shift_VP_sq);
        vif_vert_store<val_per_thread, (fwidth_rd > 0)>(buf, o, y, x_start, w);
    }
}

/* 16-bit horizontal kernel: the scale's filter row and its rounding, plain
 * tmp loads.  Same tile layout as the 8-bit kernel. */
template <int val_per_thread = 2, int fwidth, int fwidth_rd, int scale>
__device__ __forceinline__ void
filter1d_16_horizontal_kernel(VifBufferCuda buf, int w, int h, int32_t add_shift_round_HP,
                              int32_t shift_HP, filter_table_stuct vif_filt,
                              double vif_enhn_gain_limit, vif_accums *accum)
{
    vif_hori_kernel<val_per_thread, fwidth, fwidth_rd, scale, false>(
        buf, w, h, add_shift_round_HP, shift_HP, vif_filt, vif_enhn_gain_limit, accum);
}

} /* namespace */

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
