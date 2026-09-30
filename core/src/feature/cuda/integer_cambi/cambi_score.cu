/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA kernels of the CAMBI banding-detection feature extractor (T3-15 /
 *  ADR-0360), device-resident since ADR-1379 (the CUDA port of the SYCL
 *  design of ADR-1357, core/src/feature/sycl/integer_cambi_sycl.cpp).
 *
 *  Every per-frame stage of cambi.c runs here, in stream order, reading the
 *  distorted luma plane the engine already uploaded:
 *
 *    cambi_validate_kernel      cambi.c::validate_image (bpc other than 8/16)
 *    cambi_preprocess_kernel    decimate_generic_*_and_convert_to_10b through
 *                               init-time index tables + anti_dithering_filter
 *    cambi_spatial_mask_kernel  get_spatial_mask_for_index (ADR-0464 tile)
 *    cambi_decimate_kernel      decimate
 *    cambi_filter_mode_h_kernel filter_mode, horizontal pass
 *    cambi_filter_mode_v_levels_kernel
 *                               filter_mode, vertical pass; also writes the
 *                               level map Q the histograms are keyed on
 *    cambi_row_masks_kernel     per-row run / change bit masks (warp ballot)
 *    cambi_cvals_kernel         calculate_c_values + top-K radix pass 0
 *    cambi_radix_hist_kernel    top-K radix passes 1 and 2
 *    cambi_radix_scan_kernel    bucket choice of every radix pass
 *    cambi_topk_partials_kernel per-block sums above the threshold
 *    cambi_topk_final_kernel    exact 128-bit top-K sum of one scale
 *
 *  Every kernel takes one argument block by value (integer_cambi_cuda.h).
 *
 *  c-values: one thread owns one histogram column of one row chunk and
 *  slides the (2 * pad + 1)^2 window down its rows, as calculate_c_values()
 *  slides its column histograms. The uint16 cell updates are modular and
 *  commute, and cambi.c's init guard (vmaf_cambi_check_window_fits_lut)
 *  bounds every window count by 65 x 65 = 4225, so each cell the thread
 *  reads holds the true window count whatever order it applied them in.
 *  c_value_pixel() is reproduced float for float with the same reciprocal
 *  table (vmaf_cambi_reciprocal_lut, uploaded at init). Its arithmetic is
 *  one int-to-float conversion and one multiply; both are spelled with the
 *  round-to-nearest intrinsics (__int2float_rn, __fmul_rn), which nvcc never
 *  contracts into an FMA, so the result does not depend on --fmad.
 *
 *  Top-K pooling: cambi.c sums the k largest c-values in fp64 after a
 *  quick-select. Here a three-pass radix select on the IEEE bit pattern
 *  (monotonic for the non-negative c-values) finds the k-th largest value T,
 *  and sum(v > T) + (k - #(v > T)) * T is accumulated exactly in units of
 *  2^-24: every non-zero c-value lies in [0.5, 2^14), so it is an integer
 *  multiple of 2^-24. The host converts the 128-bit sum to fp64 once
 *  (vmaf_cambi_fixed_topk_mean). No kernel here uses fp64, and this file must
 *  not mention the fp64 type: core/test/test_cuda_device_resident_contract.py.
 *
 *  Precision contract: integer mask / decimate / filter / histogram stages,
 *  fp32 c-values bit-identical to cambi.c, exact top-K sums; the per-frame
 *  score equals cambi.c's whenever cambi.c's own fp64 sum is exact.
 */

#include "cuda_helper.cuh"
#include "cuda/integer_cambi_cuda.h"

namespace
{

constexpr unsigned CAMBI_FULL_WARP = 0xFFFFFFFFu;
constexpr unsigned CAMBI_WARP = 32u;
constexpr unsigned CAMBI_IMAGE_THREADS = CAMBI_CUDA_IMAGE_BLOCK_X * CAMBI_CUDA_IMAGE_BLOCK_Y;
constexpr unsigned CAMBI_ROWMASK_THREADS = CAMBI_WARP * CAMBI_CUDA_ROWMASK_ROWS;
constexpr unsigned CAMBI_BINS_PER_LANE = CAMBI_CUDA_RADIX_BINS / CAMBI_CUDA_POOL_BLOCK;
constexpr float CAMBI_FIXED_SCALE = 16777216.0f; /* 2^CAMBI_CUDA_FIXED_SHIFT */

static_assert(CAMBI_CUDA_RADIX_BINS % CAMBI_CUDA_POOL_BLOCK == 0u, "bins split evenly over lanes");
static_assert(CAMBI_CUDA_CVALS_BLOCK % CAMBI_WARP == 0u, "c-values block is whole warps");
static_assert(CAMBI_CUDA_POOL_BLOCK % CAMBI_WARP == 0u, "pooling block is whole warps");

/* Radix pass p: bits already fixed, shift and width of the digit. */
__device__ __forceinline__ uint32_t radix_known_mask(int pass)
{
    return pass == 0 ? 0u : (pass == 1 ? 0xFFE00000u : 0xFFFFFC00u);
}

__device__ __forceinline__ unsigned radix_shift(int pass)
{
    return pass == 0 ? 21u : (pass == 1 ? 10u : 0u);
}

__device__ __forceinline__ uint32_t radix_digit(uint32_t bits, int pass)
{
    const uint32_t width_mask = pass == 2 ? 0x3FFu : 0x7FFu;
    return (bits >> radix_shift(pass)) & width_mask;
}

/* Fixed point in units of 2^-24: the product is an exact power-of-two
 * scaling and, for 0 and every value in [0.5, 2^14), an integer. */
__device__ __forceinline__ uint64_t cambi_fixed(float value)
{
    return (uint64_t)__float2ull_rz(__fmul_rn(value, CAMBI_FIXED_SCALE));
}

/* Block-wide sum for a 1-D block of `threads` (a multiple of 32) threads,
 * valid in thread 0. Reuses cuda_helper.cuh's 64-bit warp_reduce; every
 * operand here stays below 2^63. */
__device__ __forceinline__ uint64_t cambi_block_sum(uint64_t value, uint64_t *warp_sums,
                                                    unsigned threads)
{
    const int64_t warp_total = warp_reduce((int64_t)value);
    if ((threadIdx.x & (CAMBI_WARP - 1u)) == 0u) {
        warp_sums[threadIdx.x / CAMBI_WARP] = (uint64_t)warp_total;
    }
    __syncthreads();
    uint64_t total = 0u;
    if (threadIdx.x == 0u) {
        for (unsigned w = 0u; w < threads / CAMBI_WARP; ++w) {
            total += warp_sums[w];
        }
    }
    return total;
}

/* Exclusive prefix sum over the CAMBI_CUDA_POOL_BLOCK threads of one block. */
__device__ __forceinline__ uint32_t cambi_block_exclusive_scan(uint32_t value,
                                                               uint32_t *warp_totals)
{
    const unsigned lane = threadIdx.x & (CAMBI_WARP - 1u);
    const unsigned warp = threadIdx.x / CAMBI_WARP;
    uint32_t inclusive = value;
    for (unsigned offset = 1u; offset < CAMBI_WARP; offset <<= 1u) {
        const uint32_t below = __shfl_up_sync(CAMBI_FULL_WARP, inclusive, offset);
        if (lane >= offset) {
            inclusive += below;
        }
    }
    if (lane == CAMBI_WARP - 1u) {
        warp_totals[warp] = inclusive;
    }
    __syncthreads();
    uint32_t base = 0u;
    for (unsigned w = 0u; w < warp; ++w) {
        base += warp_totals[w];
    }
    return base + inclusive - value;
}

} // namespace

/* ------------------------------------------------------------------ */
/* Preprocessing helpers (cambi.c::cambi_preprocessing)                */
/* ------------------------------------------------------------------ */
namespace
{

/* One luma sample of the device picture (8-bit or 16-bit storage). */
__device__ __forceinline__ unsigned cambi_src_sample(const CambiCudaPreprocArgs &a, unsigned row,
                                                     unsigned col)
{
    const unsigned char *line =
        reinterpret_cast<const unsigned char *>(a.src) + (size_t)row * a.src_pitch;
    if (a.bpc <= 8u) {
        return __ldg(line + col);
    }
    return __ldg(reinterpret_cast<const uint16_t *>(line) + col);
}

/* One output sample of decimate_generic_*_and_convert_to_10b. */
__device__ __forceinline__ unsigned cambi_preproc_sample(const CambiCudaPreprocArgs &a, unsigned i,
                                                         unsigned j)
{
    const uint32_t *ori_x = reinterpret_cast<const uint32_t *>(a.ori_x);
    const uint32_t *ori_y = reinterpret_cast<const uint32_t *>(a.ori_y);
    const unsigned row = a.same_size ? i : __ldg(ori_y + i);
    const unsigned col = a.same_size ? j : __ldg(ori_x + j);
    const unsigned v = cambi_src_sample(a, row, col);
    if (a.bpc <= 8u) {
        return v << (10u - a.bpc);
    }
    if (a.bpc == 9u) {
        return v << 1u;
    }
    const unsigned shift = a.bpc - 10u;
    const unsigned rounding = shift == 0u ? 0u : 1u << (shift - 1u);
    return (v + rounding) >> shift;
}

/* anti_dithering_filter(): the in-place row-major pass only reads samples it
 * has not overwritten yet, so it equals this out-of-place 2x2 average. */
__device__ __forceinline__ unsigned cambi_preproc_pixel(const CambiCudaPreprocArgs &a, unsigned i,
                                                        unsigned j)
{
    const unsigned here = cambi_preproc_sample(a, i, j);
    if (!a.anti_dither) {
        return here;
    }
    const bool last_row = i + 1u == a.out_height;
    const bool last_col = j + 1u == a.out_width;
    if (last_row && last_col) {
        return here;
    }
    if (last_row) {
        return (here + cambi_preproc_sample(a, i, j + 1u)) >> 1u;
    }
    if (last_col) {
        return (here + cambi_preproc_sample(a, i + 1u, j)) >> 1u;
    }
    return (here + cambi_preproc_sample(a, i, j + 1u) + cambi_preproc_sample(a, i + 1u, j) +
            cambi_preproc_sample(a, i + 1u, j + 1u)) >>
           2u;
}

__device__ __forceinline__ uint16_t mode3(uint16_t first, uint16_t second, uint16_t third)
{
    /* Two equal -> that value; all distinct -> the minimum (cambi.c::mode3). */
    if (first == second || first == third) {
        return first;
    }
    if (second == third) {
        return second;
    }
    return first < second ? (first < third ? first : third) : (second < third ? second : third);
}

} // namespace

/* ------------------------------------------------------------------ */
/* Spatial mask helpers (ADR-0464 shared-memory tile)                  */
/* ------------------------------------------------------------------ */
namespace
{

#define SMEM_HALF 3u       /* (MASK_FILTER_SIZE=7) >> 1 */
#define ZD_TILE_H 22u      /* BLOCK_Y + 2*SMEM_HALF */
#define ZD_TILE_W 22u      /* BLOCK_X + 2*SMEM_HALF */
#define ZD_TILE_STRIDE 32u /* padded row stride (uint8 cols) */

/* Zero-derivative flag of tile element k: equal to the right and the lower
 * neighbour, a missing neighbour at the image edge counting as equal, and 0
 * outside the image (get_spatial_mask_for_index()'s zero padding). */
__device__ __forceinline__ uint8_t cambi_zero_deriv(const CambiCudaMaskArgs &a, int bx, int by,
                                                    int k)
{
    const uint16_t *image = reinterpret_cast<const uint16_t *>(a.image);
    const int ti = k / (int)ZD_TILE_W;
    const int tj = k % (int)ZD_TILE_W;
    const int gy = by - (int)SMEM_HALF + ti;
    const int gx = bx - (int)SMEM_HALF + tj;
    if (gy < 0 || gy >= (int)a.height || gx < 0 || gx >= (int)a.width) {
        return 0u;
    }
    const uint16_t p = __ldg(image + (size_t)gy * a.width + (unsigned)gx);
    const unsigned r_gx = (unsigned)((gx == (int)a.width - 1) ? gx : gx + 1);
    const unsigned b_gy = (unsigned)((gy == (int)a.height - 1) ? gy : gy + 1);
    const uint16_t r = __ldg(image + (size_t)gy * a.width + r_gx);
    const uint16_t b = __ldg(image + (size_t)b_gy * a.width + (unsigned)gx);
    const int eq_r = (gx == (int)a.width - 1) || (p == r);
    const int eq_b = (gy == (int)a.height - 1) || (p == b);
    return (uint8_t)(eq_r & eq_b);
}

} // namespace

/* ------------------------------------------------------------------ */
/* c-values helpers (cambi.c::calculate_c_values)                      */
/* ------------------------------------------------------------------ */
namespace
{

/* Word `w` of a bit-mask row restricted to columns [lo, hi]. */
__device__ __forceinline__ uint32_t mask_word_in_range(const uint32_t *__restrict__ mask_row,
                                                       unsigned w, unsigned lo, unsigned hi)
{
    uint32_t bits = __ldg(mask_row + w);
    if (w == lo >> 5u) {
        bits &= ~0u << (lo & 31u);
    }
    if (w == hi >> 5u && (hi & 31u) != 31u) {
        bits &= (1u << ((hi & 31u) + 1u)) - 1u;
    }
    return bits;
}

__device__ __forceinline__ bool mask_any(const uint32_t *__restrict__ mask_row, unsigned lo,
                                         unsigned hi)
{
    uint32_t any = 0u;
    for (unsigned w = lo >> 5u; w <= hi >> 5u; ++w) {
        any |= mask_word_in_range(mask_row, w, lo, hi);
    }
    return any != 0u;
}

/* Everything one c-values thread reads or writes. The read-only planes and
 * tables go through __ldg; the histogram and c-value planes are written by
 * this kernel and read with plain loads. */
struct CvalsCtx {
    const uint16_t *q;
    const uint32_t *runs;
    const uint32_t *change;
    uint16_t *hist;
    float *cvals;
    CambiCudaSelect *select;
    const float *lut;
    const uint16_t *tvi;
    const int *weights;
    const CambiCudaCvalsArgs *a;
};

__device__ __forceinline__ CvalsCtx cvals_ctx(const CambiCudaCvalsArgs &a)
{
    CvalsCtx c;
    c.q = reinterpret_cast<const uint16_t *>(a.q);
    c.runs = reinterpret_cast<const uint32_t *>(a.runs);
    c.change = reinterpret_cast<const uint32_t *>(a.change);
    c.hist = reinterpret_cast<uint16_t *>(a.hist);
    c.cvals = reinterpret_cast<float *>(a.cvals);
    c.select = reinterpret_cast<CambiCudaSelect *>(a.select);
    c.lut = reinterpret_cast<const float *>(a.lut);
    c.tvi = reinterpret_cast<const uint16_t *>(a.tvi);
    c.weights = reinterpret_cast<const int *>(a.weights);
    c.a = &a;
    return c;
}

/* Apply `count` (+/-) to one histogram cell with uint16 wrap-around, the
 * arithmetic of increment_range() / decrement_range(). */
__device__ __forceinline__ void hist_apply(uint16_t *col_hist, unsigned width, uint16_t level,
                                           int count)
{
    if (level == CAMBI_CUDA_Q_INVALID) {
        return;
    }
    uint16_t *cell = col_hist + (size_t)level * width;
    *cell = (uint16_t)((int)*cell + count);
}

/* Add (sign +1) or remove (sign -1) row y's pixels in [lo, hi], one update
 * per run of equal levels. */
__device__ void hist_row_runs(const CvalsCtx &c, uint16_t *col_hist, unsigned y, unsigned lo,
                              unsigned hi, int sign)
{
    const unsigned width = c.a->width;
    const uint16_t *row = c.q + (size_t)y * width;
    const uint32_t *runs = c.runs + (size_t)y * c.a->words;
    unsigned start = lo;
    if (lo < hi) {
        for (unsigned w = (lo + 1u) >> 5u; w <= hi >> 5u; ++w) {
            uint32_t bits = mask_word_in_range(runs, w, lo + 1u, hi);
            for (int visited = 0; visited < 32 && bits != 0u; ++visited) {
                const unsigned x = w * 32u + (unsigned)(__ffs((int)bits) - 1);
                bits &= bits - 1u;
                hist_apply(col_hist, width, __ldg(row + start), sign * (int)(x - start));
                start = x;
            }
        }
    }
    hist_apply(col_hist, width, __ldg(row + start), sign * (int)(hi + 1u - start));
}

/* Move the window of the thread's column from row y - 1 to row y: remove row
 * y - pad - 1, add row y + pad; skipped when both agree over the window. */
__device__ void hist_slide(const CvalsCtx &c, uint16_t *col_hist, unsigned y, unsigned lo,
                           unsigned hi)
{
    const CambiCudaCvalsArgs &a = *c.a;
    if (!mask_any(c.change + (size_t)y * a.words, lo, hi)) {
        return;
    }
    if (y > a.pad) {
        hist_row_runs(c, col_hist, y - a.pad - 1u, lo, hi, -1);
    }
    if (y + a.pad < a.height) {
        hist_row_runs(c, col_hist, y + a.pad, lo, hi, 1);
    }
}

} // namespace

namespace
{

/* cambi.c::c_value_pixel for the pixel whose level-map value is q0. */
__device__ float cvals_pixel(const CvalsCtx &c, const uint16_t *col_hist, uint16_t q0)
{
    if (q0 == CAMBI_CUDA_Q_INVALID) {
        return 0.0f;
    }
    const CambiCudaCvalsArgs &a = *c.a;
    const unsigned width = a.width;
    const unsigned value = (unsigned)q0 + a.v_band_base + a.num_diffs;
    const int p0 = col_hist[(size_t)q0 * width];
    float c_value = 0.0f;
    for (unsigned d = 0u; d < a.num_diffs; ++d) {
        if (value > __ldg(c.tvi + d) || value + d + 1u <= a.vlt_luma) {
            continue;
        }
        const unsigned up = (unsigned)q0 + d + 1u;
        const int p1 = up < a.levels ? col_hist[(size_t)up * width] : 0;
        const int p2 = q0 >= d + 1u ? col_hist[(size_t)(q0 - d - 1u) * width] : 0;
        const int pm = p1 > p2 ? p1 : p2;
        /* (float)(w * p0 * pm) * reciprocal_lut[pm + p0], without contraction. */
        const float val =
            __fmul_rn(__int2float_rn(__ldg(c.weights + d) * p0 * pm), __ldg(c.lut + pm + p0));
        if (val > c_value) {
            c_value = val;
        }
    }
    return c_value;
}

/* Zero the column, then load the window of the chunk's first row. */
__device__ void cvals_prime(const CvalsCtx &c, uint16_t *col_hist, unsigned y0, unsigned lo,
                            unsigned hi)
{
    const CambiCudaCvalsArgs &a = *c.a;
    for (unsigned level = 0u; level < a.levels; ++level) {
        col_hist[(size_t)level * a.width] = 0u;
    }
    const unsigned first = y0 > a.pad ? y0 - a.pad : 0u;
    const unsigned last = y0 + a.pad < a.height ? y0 + a.pad : a.height - 1u;
    for (unsigned y = first; y <= last; ++y) {
        hist_row_runs(c, col_hist, y, lo, hi, 1);
    }
}

/* A thread's running share of top-K pass 0: the radix count of its c-values
 * (one atomic per run of equal bins) and their fixed-point sum. */
struct CvalsTally {
    uint64_t sum;
    uint32_t bin;
    uint32_t count;
};

__device__ __forceinline__ void tally_flush(const CvalsCtx &c, CvalsTally &tally)
{
    if (tally.count != 0u) {
        atomicAdd(&c.select->hist[tally.bin], tally.count);
        tally.count = 0u;
    }
}

__device__ __forceinline__ void tally_add(const CvalsCtx &c, CvalsTally &tally, float value)
{
    const uint32_t bin = radix_digit(__float_as_uint(value), 0);
    if (bin != tally.bin) {
        tally_flush(c, tally);
        tally.bin = bin;
    }
    ++tally.count;
    tally.sum += cambi_fixed(value);
}

/* One thread: histogram column `col` of row chunk `chunk`. The window of row
 * y covers rows [y - pad, y + pad] and columns [col - pad, col + pad],
 * clipped to the image, as calculate_c_values()'s first-pass / top-edge /
 * middle-slide / bottom-edge walk leaves it. */
__device__ void cvals_column(const CvalsCtx &c, unsigned chunk, unsigned col, CvalsTally &tally)
{
    const CambiCudaCvalsArgs &a = *c.a;
    const unsigned y0 = chunk * a.chunk_rows;
    if (col >= a.width || y0 >= a.height) {
        return;
    }
    const unsigned y1 = y0 + a.chunk_rows < a.height ? y0 + a.chunk_rows : a.height;
    const unsigned lo = col > a.pad ? col - a.pad : 0u;
    const unsigned hi = col + a.pad < a.width ? col + a.pad : a.width - 1u;
    uint16_t *col_hist = c.hist + (size_t)chunk * a.levels * a.width + col;
    cvals_prime(c, col_hist, y0, lo, hi);
    for (unsigned y = y0; y < y1; ++y) {
        if (y > y0) {
            hist_slide(c, col_hist, y, lo, hi);
        }
        const size_t idx = (size_t)y * a.width + col;
        const float value = cvals_pixel(c, col_hist, __ldg(c.q + idx));
        c.cvals[idx] = value;
        tally_add(c, tally, value);
    }
}

} // namespace

/* ------------------------------------------------------------------ */
/* Top-K helpers (cambi.c::spatial_pooling)                            */
/* ------------------------------------------------------------------ */
namespace
{

/* Contiguous element block of pooling block `group`. */
__device__ __forceinline__ void pool_block(const CambiCudaPoolArgs &a, unsigned group,
                                           unsigned &begin, unsigned &end)
{
    const unsigned per_group = (a.n + a.groups - 1u) / a.groups;
    begin = group * per_group;
    end = begin + per_group < a.n ? begin + per_group : a.n;
}

/* Count this block's share of the elements inside the bucket the previous
 * passes chose into `local_hist`, one atomic per run of equal bins. */
__device__ void radix_count_block(const CambiCudaPoolArgs &a, const CambiCudaSelect *select,
                                  uint32_t *local_hist)
{
    const float *cvals = reinterpret_cast<const float *>(a.cvals);
    unsigned begin = 0u;
    unsigned end = 0u;
    pool_block(a, blockIdx.x, begin, end);
    const uint32_t prefix = select->prefix;
    const uint32_t known = radix_known_mask(a.pass);
    uint32_t cur_bin = 0u;
    uint32_t cur_count = 0u;
    for (unsigned i = begin + threadIdx.x; i < end; i += CAMBI_CUDA_POOL_BLOCK) {
        const uint32_t bits = __float_as_uint(__ldg(cvals + i));
        if ((bits & known) != prefix) {
            continue;
        }
        const uint32_t bin = radix_digit(bits, a.pass);
        if (bin != cur_bin && cur_count != 0u) {
            atomicAdd(&local_hist[cur_bin], cur_count);
            cur_count = 0u;
        }
        cur_bin = bin;
        ++cur_count;
    }
    if (cur_count != 0u) {
        atomicAdd(&local_hist[cur_bin], cur_count);
    }
}

/* 128-bit accumulator: value = hi * 2^64 + lo. */
struct U128 {
    uint64_t lo;
    uint64_t hi;
};

/* hi32 * 2^32 + lo32 as a 128-bit value (both halves < 2^64). */
__device__ __forceinline__ U128 u128_from_halves(uint64_t hi32_sum, uint64_t lo32_sum)
{
    const uint64_t shifted = hi32_sum << 32u;
    U128 r{shifted + lo32_sum, hi32_sum >> 32u};
    r.hi += r.lo < shifted ? 1u : 0u;
    return r;
}

__device__ __forceinline__ U128 u128_add(U128 x, U128 y)
{
    U128 r{x.lo + y.lo, x.hi + y.hi};
    r.hi += r.lo < x.lo ? 1u : 0u;
    return r;
}

} // namespace

extern "C" {

/* ------------------------------------------------------------------ */
/* Preprocessing (cambi.c::cambi_preprocessing)                        */
/* ------------------------------------------------------------------ */

/* cambi.c::validate_image: flag any sample above (1 << bpc) - 1. The host
 * launches it only for a bit depth other than 8 and 16, as cambi.c. */
__global__ void __launch_bounds__(CAMBI_IMAGE_THREADS)
    cambi_validate_kernel(const CambiCudaPreprocArgs a)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= a.src_width || y >= a.src_height) {
        return;
    }
    if (cambi_src_sample(a, y, x) > (1u << a.bpc) - 1u) {
        CambiCudaResults *results = reinterpret_cast<CambiCudaResults *>(a.results);
        atomicOr(&results->status, CAMBI_CUDA_STATUS_INVALID_INPUT);
    }
}

/* Convert to 10 bit (resizing through the init-time index tables), then the
 * anti-dither 2x2 average. */
__global__ void __launch_bounds__(CAMBI_IMAGE_THREADS)
    cambi_preprocess_kernel(const CambiCudaPreprocArgs a)
{
    const unsigned j = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned i = blockIdx.y * blockDim.y + threadIdx.y;
    if (j >= a.out_width || i >= a.out_height) {
        return;
    }
    uint16_t *dst = reinterpret_cast<uint16_t *>(a.dst);
    dst[(size_t)i * a.out_width + j] = (uint16_t)cambi_preproc_pixel(a, i, j);
}

/* ------------------------------------------------------------------ */
/* Spatial mask (cambi.c::get_spatial_mask_for_index).                 */
/*                                                                     */
/* The zero-derivative flag summed over the zero-padded 7x7 box and    */
/* compared against mask_index. A 16x16 block computes the 22x22 flag  */
/* footprint it needs into shared memory, then every thread sums its   */
/* box from shared memory. Integer only, ULP = 0.                      */
/* ------------------------------------------------------------------ */
__global__ void __launch_bounds__(CAMBI_IMAGE_THREADS)
    cambi_spatial_mask_kernel(const CambiCudaMaskArgs a)
{
    __shared__ uint8_t zd_tile[ZD_TILE_H][ZD_TILE_STRIDE];

    const int bx = (int)(blockIdx.x * blockDim.x);
    const int by = (int)(blockIdx.y * blockDim.y);
    const int lx = (int)threadIdx.x;
    const int ly = (int)threadIdx.y;
    const int tid = ly * (int)blockDim.x + lx;

    for (int k = tid; k < (int)(ZD_TILE_H * ZD_TILE_W); k += (int)CAMBI_IMAGE_THREADS) {
        zd_tile[k / (int)ZD_TILE_W][k % (int)ZD_TILE_W] = cambi_zero_deriv(a, bx, by, k);
    }
    __syncthreads();

    const int x = bx + lx;
    const int y = by + ly;
    if (x >= (int)a.width || y >= (int)a.height) {
        return;
    }
    unsigned box_sum = 0u;
#pragma unroll
    for (int dy = 0; dy <= 2 * (int)SMEM_HALF; dy++) {
#pragma unroll
        for (int dx = 0; dx <= 2 * (int)SMEM_HALF; dx++) {
            box_sum += (unsigned)zd_tile[ly + dy][lx + dx];
        }
    }
    uint16_t *mask = reinterpret_cast<uint16_t *>(a.mask);
    mask[(size_t)(unsigned)y * a.width + (unsigned)x] =
        (uint16_t)(box_sum > a.mask_index ? 1u : 0u);
}

/* ------------------------------------------------------------------ */
/* Per-scale image stages.                                             */
/* ------------------------------------------------------------------ */

/* Strict stride-2 subsample, cambi.c::decimate (in place there; that walk
 * only reads samples it has not overwritten). */
__global__ void __launch_bounds__(CAMBI_IMAGE_THREADS)
    cambi_decimate_kernel(const CambiCudaDecimateArgs a)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= a.out_width || y >= a.out_height) {
        return;
    }
    const uint16_t *src = reinterpret_cast<const uint16_t *>(a.src);
    uint16_t *dst = reinterpret_cast<uint16_t *>(a.dst);
    dst[(size_t)y * a.out_width + x] = __ldg(src + (size_t)(y * 2u) * a.src_stride + x * 2u);
}

/* filter_mode, horizontal pass: edge columns keep their value because
 * mode3(a, a, b) == a. */
__global__ void __launch_bounds__(CAMBI_IMAGE_THREADS)
    cambi_filter_mode_h_kernel(const CambiCudaFilterArgs a)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= a.width || y >= a.height) {
        return;
    }
    const uint16_t *row = reinterpret_cast<const uint16_t *>(a.image) + (size_t)y * a.width;
    const unsigned left = x > 0u ? x - 1u : 0u;
    const unsigned right = x + 1u < a.width ? x + 1u : a.width - 1u;
    uint16_t *out = reinterpret_cast<uint16_t *>(a.filtered_h);
    out[(size_t)y * a.width + x] = mode3(__ldg(row + left), __ldg(row + x), __ldg(row + right));
}

/* filter_mode, vertical pass, plus the level map. cambi.c::filter_mode
 * writes rows 1 .. height-2 only, so the first and last rows keep their
 * pre-filter value. Q is the histogram row calculate_c_values() files the
 * pixel under, or CAMBI_CUDA_Q_INVALID when it is masked out or outside
 * the band. Each thread reads and writes only its own image pixel. */
__global__ void __launch_bounds__(CAMBI_IMAGE_THREADS)
    cambi_filter_mode_v_levels_kernel(const CambiCudaFilterArgs a)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= a.width || y >= a.height) {
        return;
    }
    const uint16_t *filtered_h = reinterpret_cast<const uint16_t *>(a.filtered_h);
    uint16_t *image = reinterpret_cast<uint16_t *>(a.image);
    const size_t idx = (size_t)y * a.width + x;
    uint16_t value = image[idx];
    if (y > 0u && y + 1u < a.height) {
        value = mode3(__ldg(filtered_h + idx - a.width), __ldg(filtered_h + idx),
                      __ldg(filtered_h + idx + a.width));
        image[idx] = value;
    }
    const uint16_t compact = (uint16_t)(value - a.v_band_base);
    const uint16_t *mask = reinterpret_cast<const uint16_t *>(a.mask);
    uint16_t *q = reinterpret_cast<uint16_t *>(a.q);
    q[idx] = (__ldg(mask + idx) != 0u && compact < a.v_band_size) ? compact :
                                                                    (uint16_t)CAMBI_CUDA_Q_INVALID;
}

/* ------------------------------------------------------------------ */
/* c-values (cambi.c::calculate_c_values).                             */
/* ------------------------------------------------------------------ */

/* Per-row bit masks over the level map, 32 columns per word:
 *   runs[y]   bit x (x > 0) set where Q[y][x] != Q[y][x - 1] (run starts);
 *   change[y] bit x set where the row leaving the window of row y
 *             (y - pad - 1) and the row entering it (y + pad) differ at x,
 *             an absent row reading as CAMBI_CUDA_Q_INVALID.
 * One warp covers 32 columns of one row, so a ballot is the mask word. */
__global__ void __launch_bounds__(CAMBI_ROWMASK_THREADS)
    cambi_row_masks_kernel(const CambiCudaCvalsArgs a)
{
    const uint16_t *q = reinterpret_cast<const uint16_t *>(a.q);
    const unsigned x = blockIdx.x * CAMBI_WARP + threadIdx.x;
    const unsigned y = blockIdx.y * CAMBI_CUDA_ROWMASK_ROWS + threadIdx.y;
    bool run_start = false;
    bool differs = false;
    if (x < a.width && y < a.height) {
        const uint16_t *row = q + (size_t)y * a.width;
        run_start = x > 0u && __ldg(row + x) != __ldg(row + x - 1u);
        const uint16_t leaving = y > a.pad ? __ldg(q + (size_t)(y - a.pad - 1u) * a.width + x) :
                                             (uint16_t)CAMBI_CUDA_Q_INVALID;
        const uint16_t entering = y + a.pad < a.height ?
                                      __ldg(q + (size_t)(y + a.pad) * a.width + x) :
                                      (uint16_t)CAMBI_CUDA_Q_INVALID;
        differs = leaving != entering;
    }
    const uint32_t runs_word = __ballot_sync(CAMBI_FULL_WARP, run_start);
    const uint32_t change_word = __ballot_sync(CAMBI_FULL_WARP, differs);
    if (threadIdx.x == 0u && y < a.height) {
        reinterpret_cast<uint32_t *>(a.runs)[(size_t)y * a.words + blockIdx.x] = runs_word;
        reinterpret_cast<uint32_t *>(a.change)[(size_t)y * a.words + blockIdx.x] = change_word;
    }
}

/* c-values plus top-K pass 0: the radix histogram of every c-value and one
 * fixed-point partial sum per block (the whole top-K sum whenever the
 * threshold resolves to 0). Grid: x = column blocks, y = row chunks. */
__global__ void __launch_bounds__(CAMBI_CUDA_CVALS_BLOCK)
    cambi_cvals_kernel(const CambiCudaCvalsArgs a)
{
    __shared__ uint64_t warp_sums[CAMBI_CUDA_CVALS_BLOCK / CAMBI_WARP];
    const CvalsCtx c = cvals_ctx(a);
    CvalsTally tally{0u, 0u, 0u};
    cvals_column(c, blockIdx.y, blockIdx.x * CAMBI_CUDA_CVALS_BLOCK + threadIdx.x, tally);
    tally_flush(c, tally);
    const uint64_t total = cambi_block_sum(tally.sum, warp_sums, CAMBI_CUDA_CVALS_BLOCK);
    if (threadIdx.x == 0u) {
        uint64_t *partials = reinterpret_cast<uint64_t *>(a.partials);
        partials[(size_t)blockIdx.y * gridDim.x + blockIdx.x] = total;
    }
}

/* ------------------------------------------------------------------ */
/* Exact top-K pooling (cambi.c::spatial_pooling).                     */
/* ------------------------------------------------------------------ */

/* Radix histogram of passes 1 and 2 (pass 0 is counted by the c-values
 * kernel), restricted to the bucket the previous passes chose. The resolved
 * flag is uniform, so either every thread returns or none does. */
__global__ void __launch_bounds__(CAMBI_CUDA_POOL_BLOCK)
    cambi_radix_hist_kernel(const CambiCudaPoolArgs a)
{
    __shared__ uint32_t local_hist[CAMBI_CUDA_RADIX_BINS];
    CambiCudaSelect *select = reinterpret_cast<CambiCudaSelect *>(a.select);
    if (select->resolved != 0u) {
        return;
    }
    for (unsigned b = threadIdx.x; b < CAMBI_CUDA_RADIX_BINS; b += CAMBI_CUDA_POOL_BLOCK) {
        local_hist[b] = 0u;
    }
    __syncthreads();
    radix_count_block(a, select, local_hist);
    __syncthreads();
    for (unsigned b = threadIdx.x; b < CAMBI_CUDA_RADIX_BINS; b += CAMBI_CUDA_POOL_BLOCK) {
        if (local_hist[b] != 0u) {
            atomicAdd(&select->hist[b], local_hist[b]);
        }
    }
}

/* One block. Lane l owns bins [2047 - 8l - 7, 2047 - 8l], visited high to
 * low. The lane whose range holds the k-th largest element records its bin
 * and the rank left inside it; every lane then clears its bins for the next
 * pass. */
__global__ void __launch_bounds__(CAMBI_CUDA_POOL_BLOCK)
    cambi_radix_scan_kernel(const CambiCudaPoolArgs a)
{
    __shared__ uint32_t warp_totals[CAMBI_CUDA_POOL_BLOCK / CAMBI_WARP];
    CambiCudaSelect *select = reinterpret_cast<CambiCudaSelect *>(a.select);
    if (select->resolved != 0u) {
        return;
    }
    const unsigned top = CAMBI_CUDA_RADIX_BINS - 1u - threadIdx.x * CAMBI_BINS_PER_LANE;
    const uint32_t k = a.pass == 0 ? a.topk : select->k_next[a.pass - 1];
    uint32_t mine = 0u;
    for (unsigned j = 0u; j < CAMBI_BINS_PER_LANE; ++j) {
        mine += select->hist[top - j];
    }
    const uint32_t before = cambi_block_exclusive_scan(mine, warp_totals);
    if (before < k && k <= before + mine) {
        uint32_t cum = before;
        for (unsigned j = 0u; j < CAMBI_BINS_PER_LANE; ++j) {
            const uint32_t count = select->hist[top - j];
            if (cum + count >= k) {
                select->prefix |= (top - j) << radix_shift(a.pass);
                select->k_next[a.pass] = k - cum;
                select->resolved = (a.pass == 0 && top - j == 0u) ? 1u : 0u;
                break;
            }
            cum += count;
        }
    }
    for (unsigned j = 0u; j < CAMBI_BINS_PER_LANE; ++j) {
        select->hist[top - j] = 0u;
    }
}

/* Per-block sum of every element strictly above the threshold. Not needed
 * when the threshold resolved to 0: the c-values kernel's partials already
 * hold the sum of every element. */
__global__ void __launch_bounds__(CAMBI_CUDA_POOL_BLOCK)
    cambi_topk_partials_kernel(const CambiCudaPoolArgs a)
{
    __shared__ uint64_t warp_sums[CAMBI_CUDA_POOL_BLOCK / CAMBI_WARP];
    const CambiCudaSelect *select = reinterpret_cast<const CambiCudaSelect *>(a.select);
    if (select->resolved != 0u) {
        return;
    }
    const float *cvals = reinterpret_cast<const float *>(a.cvals);
    unsigned begin = 0u;
    unsigned end = 0u;
    pool_block(a, blockIdx.x, begin, end);
    const uint32_t threshold = select->prefix;
    uint64_t sum = 0u;
    for (unsigned i = begin + threadIdx.x; i < end; i += CAMBI_CUDA_POOL_BLOCK) {
        const float value = __ldg(cvals + i);
        sum += __float_as_uint(value) > threshold ? cambi_fixed(value) : 0u;
    }
    const uint64_t total = cambi_block_sum(sum, warp_sums, CAMBI_CUDA_POOL_BLOCK);
    if (threadIdx.x == 0u) {
        reinterpret_cast<uint64_t *>(a.partials)[blockIdx.x] = total;
    }
}

/* One block: sum the per-block partials (each < 2^64, split into 32-bit
 * halves so no running sum can wrap) and the k_next copies of the threshold
 * value into the scale's 128-bit result. */
__global__ void __launch_bounds__(CAMBI_CUDA_POOL_BLOCK)
    cambi_topk_final_kernel(const CambiCudaPoolArgs a)
{
    __shared__ uint64_t lo_sums[CAMBI_CUDA_POOL_BLOCK / CAMBI_WARP];
    __shared__ uint64_t hi_sums[CAMBI_CUDA_POOL_BLOCK / CAMBI_WARP];
    const CambiCudaSelect *select = reinterpret_cast<const CambiCudaSelect *>(a.select);
    const uint64_t *partials = reinterpret_cast<const uint64_t *>(a.partials);
    const bool resolved = select->resolved != 0u;
    const unsigned count = resolved ? a.cvals_groups : a.groups;
    uint64_t lo32 = 0u;
    uint64_t hi32 = 0u;
    for (unsigned g = threadIdx.x; g < count; g += CAMBI_CUDA_POOL_BLOCK) {
        const uint64_t partial = partials[g];
        lo32 += partial & 0xFFFFFFFFu;
        hi32 += partial >> 32u;
    }
    lo32 = cambi_block_sum(lo32, lo_sums, CAMBI_CUDA_POOL_BLOCK);
    hi32 = cambi_block_sum(hi32, hi_sums, CAMBI_CUDA_POOL_BLOCK);
    if (threadIdx.x != 0u) {
        return;
    }
    const uint64_t t_fixed = resolved ? 0u : cambi_fixed(__uint_as_float(select->prefix));
    const uint64_t k_rem = select->k_next[CAMBI_CUDA_RADIX_PASSES - 1];
    const U128 ties = u128_from_halves((t_fixed >> 32u) * k_rem, (t_fixed & 0xFFFFFFFFu) * k_rem);
    const U128 total = u128_add(u128_from_halves(hi32, lo32), ties);
    CambiCudaResults *results = reinterpret_cast<CambiCudaResults *>(a.results);
    results->sum_lo[a.scale] = total.lo;
    results->sum_hi[a.scale] = total.hi;
}

} /* extern "C" */
