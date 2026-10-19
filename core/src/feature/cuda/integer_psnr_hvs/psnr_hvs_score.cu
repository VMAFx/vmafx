/**
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-2-Clause
 *
 *  CUDA compute kernel for the psnr_hvs feature extractor
 *  (T7-23 / batch 2 part 3b / ADR-0188 / ADR-0191 / ADR-1369 / ADR-1397).
 *
 *  Reads raw samples directly from device picture planes with pitched row
 *  strides (ADR-1369). Two threads per 8x8 block (one reference, one
 *  distorted), warp-shuffle exchange of block stats, in-place integer DCT
 *  in shared memory, and one launch across all active planes.
 *
 *  ADR-1397: the kernel reproduces calc_psnrhvs()
 *  (third_party/xiph/psnr_hvs.c) operation for operation and stores the 64
 *  masked coefficient errors of every block, in the CPU's row-major order,
 *  instead of a per-block sum. The host adds them into one running float in
 *  the CPU's order (vmaf_psnr_hvs_plane_score), so the scores are the CPU's
 *  bit for bit. Three things carry that and must stay:
 *    - the masking table is the CPU's: (csf * 0.3885746225901003)^2 taken
 *      in double and stored as float (hvs_mask_value, evaluated at compile
 *      time);
 *    - the masking threshold takes the float product of the CPU's
 *      statement and a double square root before it is stored as float
 *      (hvs_threshold, ADR-1488);
 *    - the TU is built with --fmad=false (core/src/meson.build), because
 *      the CPU build never contracts a multiply and an add.
 */

#include <cuda_runtime.h>
#include "cuda/integer_psnr_hvs_cuda.h"

/* Contrast-sensitivity tables (csf_y / csf_cb420 / csf_cr420 of
 * third_party/xiph/psnr_hvs.c) and the masking tables calc_psnrhvs() derives
 * from them. */
namespace
{
struct HvsTables {
    float csf[PSNR_HVS_NUM_PLANES][PSNR_HVS_TERMS];
    float mask[PSNR_HVS_NUM_PLANES][PSNR_HVS_TERMS];
};

/* calc_psnrhvs(): mask = (csf * 0.3885746225901003) * (csf * 0.3885746225901003),
 * a double product stored as float. */
constexpr float hvs_mask_value(float csf)
{
    const double scaled = (double)csf * 0.3885746225901003;
    return (float)(scaled * scaled);
}

constexpr float HVS_CSF[PSNR_HVS_NUM_PLANES][PSNR_HVS_TERMS] = {
    /* Y */
    {1.6193873005f,   2.2901594831f,   2.08509755623f,  1.48366094411f,  1.00227514334f,
     0.678296995242f, 0.466224900598f, 0.3265091542f,   2.2901594831f,   1.94321815382f,
     2.04793073064f,  1.68731108984f,  1.2305666963f,   0.868920337363f, 0.61280991668f,
     0.436405793551f, 2.08509755623f,  2.04793073064f,  1.34329019223f,  1.09205635862f,
     0.875748795257f, 0.670882927016f, 0.501731932449f, 0.372504254596f, 1.48366094411f,
     1.68731108984f,  1.09205635862f,  0.772819797575f, 0.605636379554f, 0.48309405692f,
     0.380429446972f, 0.295774038565f, 1.00227514334f,  1.2305666963f,   0.875748795257f,
     0.605636379554f, 0.448996256676f, 0.352889268808f, 0.283006984131f, 0.226951348204f,
     0.678296995242f, 0.868920337363f, 0.670882927016f, 0.48309405692f,  0.352889268808f,
     0.27032073436f,  0.215017739696f, 0.17408067321f,  0.466224900598f, 0.61280991668f,
     0.501731932449f, 0.380429446972f, 0.283006984131f, 0.215017739696f, 0.168869545842f,
     0.136153931001f, 0.3265091542f,   0.436405793551f, 0.372504254596f, 0.295774038565f,
     0.226951348204f, 0.17408067321f,  0.136153931001f, 0.109083846276f},
    /* Cb */
    {1.91113096927f,  2.46074210438f,  1.18284184739f,  1.14982565193f,  1.05017074788f,
     0.898018824055f, 0.74725392039f,  0.615105596242f, 2.46074210438f,  1.58529308355f,
     1.21363250036f,  1.38190029285f,  1.33100189972f,  1.17428548929f,  0.996404342439f,
     0.830890433625f, 1.18284184739f,  1.21363250036f,  0.978712413627f, 1.02624506078f,
     1.03145147362f,  0.960060382087f, 0.849823426169f, 0.731221236837f, 1.14982565193f,
     1.38190029285f,  1.02624506078f,  0.861317501629f, 0.801821139099f, 0.751437590932f,
     0.685398513368f, 0.608694761374f, 1.05017074788f,  1.33100189972f,  1.03145147362f,
     0.801821139099f, 0.676555426187f, 0.605503172737f, 0.55002013668f,  0.495804539034f,
     0.898018824055f, 1.17428548929f,  0.960060382087f, 0.751437590932f, 0.605503172737f,
     0.514674450957f, 0.454353482512f, 0.407050308965f, 0.74725392039f,  0.996404342439f,
     0.849823426169f, 0.685398513368f, 0.55002013668f,  0.454353482512f, 0.389234902883f,
     0.342353999733f, 0.615105596242f, 0.830890433625f, 0.731221236837f, 0.608694761374f,
     0.495804539034f, 0.407050308965f, 0.342353999733f, 0.295530605237f},
    /* Cr */
    {2.03871978502f,  2.62502345193f,  1.26180942886f,  1.11019789803f,  1.01397751469f,
     0.867069376285f, 0.721500455585f, 0.593906509971f, 2.62502345193f,  1.69112867013f,
     1.17180569821f,  1.3342742857f,   1.28513006198f,  1.13381474809f,  0.962064122248f,
     0.802254508198f, 1.26180942886f,  1.17180569821f,  0.944981930573f, 0.990876405848f,
     0.995903384143f, 0.926972725286f, 0.820534991409f, 0.706020324706f, 1.11019789803f,
     1.3342742857f,   0.990876405848f, 0.831632933426f, 0.77418706195f,  0.725539939514f,
     0.661776842059f, 0.587716619023f, 1.01397751469f,  1.28513006198f,  0.995903384143f,
     0.77418706195f,  0.653238524286f, 0.584635025748f, 0.531064164893f, 0.478717061273f,
     0.867069376285f, 1.13381474809f,  0.926972725286f, 0.725539939514f, 0.584635025748f,
     0.496936637883f, 0.438694579826f, 0.393021669543f, 0.721500455585f, 0.962064122248f,
     0.820534991409f, 0.661776842059f, 0.531064164893f, 0.438694579826f, 0.375820256136f,
     0.330555063063f, 0.593906509971f, 0.802254508198f, 0.706020324706f, 0.587716619023f,
     0.478717061273f, 0.393021669543f, 0.330555063063f, 0.285345396658f}};

constexpr HvsTables hvs_make_tables()
{
    HvsTables tables{};
    for (int plane = 0; plane < PSNR_HVS_NUM_PLANES; plane++) {
        for (int index = 0; index < PSNR_HVS_TERMS; index++) {
            tables.csf[plane][index] = HVS_CSF[plane][index];
        }
    }
    for (int plane = 0; plane < PSNR_HVS_NUM_PLANES; plane++) {
        for (int index = 0; index < PSNR_HVS_TERMS; index++) {
            tables.mask[plane][index] = hvs_mask_value(tables.csf[plane][index]);
        }
    }
    return tables;
}

/* Constant-initialised: nvcc emits both tables as static data. */
__device__ constexpr HvsTables HVS_TABLES = hvs_make_tables();
} // namespace

extern "C" {

namespace
{
/* Round-toward-zero right shift — matches OD_UNBIASED_RSHIFT32
 * macro in xiph/psnr_hvs.c. */
__device__ inline int od_dct_rshift(int a, int b)
{
    return (int)(((unsigned int)a >> (32 - b)) + (unsigned int)a) >> b;
}

/* Forward 8-point DCT — port of od_bin_fdct8 from
 * libvmaf/src/feature/third_party/xiph/psnr_hvs.c. */
__device__ void od_bin_fdct8(int &y0, int &y1, int &y2, int &y3, int &y4, int &y5, int &y6, int &y7,
                             int x0, int x1, int x2, int x3, int x4, int x5, int x6, int x7)
{
    int t0 = x0;
    int t4 = x1;
    int t2 = x2;
    int t6 = x3;
    int t7 = x4;
    int t3 = x5;
    int t5 = x6;
    int t1 = x7;
    t1 = t0 - t1;
    const int t1h = od_dct_rshift(t1, 1);
    t0 -= t1h;
    t4 += t5;
    const int t4h = od_dct_rshift(t4, 1);
    t5 -= t4h;
    t3 = t2 - t3;
    t2 -= od_dct_rshift(t3, 1);
    t6 += t7;
    const int t6h = od_dct_rshift(t6, 1);
    t7 = t6h - t7;
    t0 += t6h;
    t6 = t0 - t6;
    t2 = t4h - t2;
    t4 = t2 - t4;
    t0 -= (t4 * 13573 + 16384) >> 15;
    t4 += (t0 * 11585 + 8192) >> 14;
    t0 -= (t4 * 13573 + 16384) >> 15;
    t6 -= (t2 * 21895 + 16384) >> 15;
    t2 += (t6 * 15137 + 8192) >> 14;
    t6 -= (t2 * 21895 + 16384) >> 15;
    t3 += (t5 * 19195 + 16384) >> 15;
    t5 += (t3 * 11585 + 8192) >> 14;
    t3 -= (t5 * 7489 + 4096) >> 13;
    t7 = od_dct_rshift(t5, 1) - t7;
    t5 -= t7;
    t3 = t1h - t3;
    t1 -= t3;
    t7 += (t1 * 3227 + 16384) >> 15;
    t1 -= (t7 * 6393 + 16384) >> 15;
    t7 += (t1 * 3227 + 16384) >> 15;
    t5 += (t3 * 2485 + 4096) >> 13;
    t3 -= (t5 * 18205 + 16384) >> 15;
    t5 += (t3 * 2485 + 4096) >> 13;
    y0 = t0;
    y1 = t1;
    y2 = t2;
    y3 = t3;
    y4 = t4;
    y5 = t5;
    y6 = t6;
    y7 = t7;
}

__device__ inline void hvs_fdct8_column(int *slm, size_t base, size_t i)
{
    int y0;
    int y1;
    int y2;
    int y3;
    int y4;
    int y5;
    int y6;
    int y7;
    od_bin_fdct8(y0, y1, y2, y3, y4, y5, y6, y7, slm[base + i], slm[base + 8 + i],
                 slm[base + 16 + i], slm[base + 24 + i], slm[base + 32 + i], slm[base + 40 + i],
                 slm[base + 48 + i], slm[base + 56 + i]);
    slm[base + i] = y0;
    slm[base + 8 + i] = y1;
    slm[base + 16 + i] = y2;
    slm[base + 24 + i] = y3;
    slm[base + 32 + i] = y4;
    slm[base + 40 + i] = y5;
    slm[base + 48 + i] = y6;
    slm[base + 56 + i] = y7;
}

__device__ inline void hvs_fdct8_row(int *slm, size_t base, size_t i)
{
    const size_t row = base + (i * 8);
    int y0;
    int y1;
    int y2;
    int y3;
    int y4;
    int y5;
    int y6;
    int y7;
    od_bin_fdct8(y0, y1, y2, y3, y4, y5, y6, y7, slm[row + 0], slm[row + 1], slm[row + 2],
                 slm[row + 3], slm[row + 4], slm[row + 5], slm[row + 6], slm[row + 7]);
    slm[row + 0] = y0;
    slm[row + 1] = y1;
    slm[row + 2] = y2;
    slm[row + 3] = y3;
    slm[row + 4] = y4;
    slm[row + 5] = y5;
    slm[row + 6] = y6;
    slm[row + 7] = y7;
}

__device__ inline void hvs_fdct8x8(int *slm, size_t base)
{
#pragma unroll
    for (size_t i = 0; i < 8; i++) {
        hvs_fdct8_column(slm, base, i);
    }
#pragma unroll
    for (size_t i = 0; i < 8; i++) {
        hvs_fdct8_row(slm, base, i);
    }
}

struct HvsLaneBlock {
    const void *src;
    size_t stride;
    size_t origin_x;
    size_t origin_y;
    unsigned block;
    int plane;
};

__device__ inline HvsLaneBlock hvs_locate(const PsnrHvsKernelArgs &args, unsigned block,
                                          bool is_dist)
{
    int plane = 0;
    const int n_planes_i = (int)args.n_planes;
#pragma unroll
    for (int p = 1; p < PSNR_HVS_NUM_PLANES; p++) {
        if (p < n_planes_i && block >= args.plane[p].first_block) {
            plane = p;
        }
    }
    const PsnrHvsPlaneArgs &geometry = args.plane[plane];
    const unsigned in_plane = block - geometry.first_block;
    const size_t origin_x = (size_t)(in_plane % geometry.blocks_x) * PSNR_HVS_STEP;
    const size_t origin_y = (size_t)(in_plane / geometry.blocks_x) * PSNR_HVS_STEP;
    /* Filled field by field: nvcc's MSVC host frontend rejects designated
     * initializers in device code (preflight --stage msvcism). */
    HvsLaneBlock lane;
    lane.src = is_dist ? geometry.dist : geometry.ref;
    lane.stride = is_dist ? geometry.dist_stride : geometry.ref_stride;
    lane.origin_x = origin_x;
    lane.origin_y = origin_y;
    lane.block = block;
    lane.plane = plane;
    return lane;
}

__device__ inline void hvs_load_block(int *slm, size_t base, const HvsLaneBlock &lane, bool wide)
{
    for (size_t row = 0; row < 8; row++) {
        const size_t y = lane.origin_y + row;
        // SAFETY: lane.src points to valid GPU frame buffer, stride is row stride in bytes.
        const char *row_ptr = (const char *)lane.src + y * lane.stride;
        for (size_t col = 0; col < 8; col++) {
            const size_t x = lane.origin_x + col;
            slm[base + row * 8 + col] =
                wide ? (int)((const uint16_t *)row_ptr)[x] : (int)((const uint8_t *)row_ptr)[x];
        }
    }
}

__device__ inline float hvs_variance_ratio(const int *block, size_t base)
{
    float means[4] = {0.f, 0.f, 0.f, 0.f};
    float global_mean = 0.f;
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            const int subgroup = ((row & 12) >> 2) + ((col & 12) >> 1);
            const int index = (row * 8) + col;
            global_mean += (float)block[base + index];
            means[subgroup] += (float)block[base + index];
        }
    }
    global_mean /= 64.f;
    means[0] /= 16.f;
    means[1] /= 16.f;
    means[2] /= 16.f;
    means[3] /= 16.f;
    float variances[4] = {0.f, 0.f, 0.f, 0.f};
    float global_variance = 0.f;
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            const int subgroup = ((row & 12) >> 2) + ((col & 12) >> 1);
            const int index = (row * 8) + col;
            const float global_delta = (float)block[base + index] - global_mean;
            const float subgroup_delta = (float)block[base + index] - means[subgroup];
            global_variance += global_delta * global_delta;
            variances[subgroup] += subgroup_delta * subgroup_delta;
        }
    }
    global_variance *= 1.f / 63.f * 64.f;
    variances[0] *= 1.f / 15.f * 16.f;
    variances[1] *= 1.f / 15.f * 16.f;
    variances[2] *= 1.f / 15.f * 16.f;
    variances[3] *= 1.f / 15.f * 16.f;
    if (global_variance > 0.f) {
        global_variance =
            (variances[0] + variances[1] + variances[2] + variances[3]) / global_variance;
    }
    return global_variance;
}

__device__ inline float hvs_mask_energy(const int *block, size_t base, int plane)
{
    float energy = 0.f;
    for (int row = 0; row < 8; row++) {
        const int first_col = (row == 0) ? 1 : 0;
        for (int col = first_col; col < 8; col++) {
            const int index = (row * 8) + col;
            const int coefficient = block[base + index];
            energy += (float)(coefficient * coefficient) * HVS_TABLES.mask[plane][index];
        }
    }
    return energy;
}

/* calc_psnrhvs(): s_mask = sqrt(s_mask * s_gvar) / 32.f, stored as float
 * (ADR-1488). The product is a float product there, so it is one here (a
 * plain operator rounds once: the fatbin is built with --fmad=false); the
 * square root is double there, so it is here (sqrt.rn.f64 is correctly
 * rounded). */
__device__ inline float hvs_threshold(float energy, float ratio)
{
    const float product = energy * ratio;
    return (float)(sqrt((double)product) / 32.0);
}

/* The 64 values calc_psnrhvs() adds to its running sum for one block, in its
 * order (row-major). */
__device__ inline uint64_t hvs_store_terms(float *terms, const int *block, size_t ref_base,
                                           size_t dist_base, float threshold, int plane)
{
    uint64_t mask = 0ULL;
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            const int index = (row * 8) + col;
            const float csf = HVS_TABLES.csf[plane][index];
            float error = (float)abs(block[ref_base + index] - block[dist_base + index]);
            if (index != 0) {
                const float masking = threshold / HVS_TABLES.mask[plane][index];
                error = error < masking ? 0.f : error - masking;
            }
            terms[index] = (error * csf) * (error * csf);
            if (terms[index] != 0.0f) {
                mask |= (1ULL << index);
            }
        }
    }
    return mask;
}

} // namespace

__launch_bounds__(64) __global__ void psnr_hvs(PsnrHvsKernelArgs args)
{
    __shared__ int s_block[PSNR_HVS_WG * PSNR_HVS_LANE_STRIDE];

    const size_t id = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    const bool active = id < 2U * (size_t)args.total_blocks;
    const bool is_dist = (id & 1U) != 0U;
    const HvsLaneBlock lane = hvs_locate(args, active ? (unsigned)(id >> 1) : 0U, is_dist);
    const size_t base = (size_t)threadIdx.x * PSNR_HVS_LANE_STRIDE;

    hvs_load_block(s_block, base, lane, args.wide != 0);
    const float ratio = hvs_variance_ratio(s_block, base);
    hvs_fdct8x8(s_block, base);
    const float energy = hvs_mask_energy(s_block, base, lane.plane);

    const float partner_ratio = __shfl_xor_sync(0xffffffff, ratio, 1);
    const float partner_energy = __shfl_xor_sync(0xffffffff, energy, 1);
    __syncwarp();

    if (active && !is_dist) {
        float threshold = hvs_threshold(energy, ratio);
        const float dist_threshold = hvs_threshold(partner_energy, partner_ratio);
        if (dist_threshold > threshold) {
            threshold = dist_threshold;
        }
        // SAFETY: args.terms holds PSNR_HVS_TERMS floats per block and
        // lane.block < args.total_blocks on an active lane.
        const uint64_t mask =
            hvs_store_terms(args.terms + (size_t)lane.block * PSNR_HVS_TERMS, s_block, base,
                            base + PSNR_HVS_LANE_STRIDE, threshold, lane.plane);
        if (args.block_masks != nullptr) {
            args.block_masks[lane.block] = mask;
        }
        if (args.block_counts != nullptr) {
            args.block_counts[lane.block] = (uint32_t)__popcll((unsigned long long)mask);
        }
    }
}

namespace
{
__device__ inline uint32_t warp_scan_inclusive(uint32_t val)
{
    const int lane = (int)(threadIdx.x & 31u);
#pragma unroll
    for (int offset = 1; offset < 32; offset <<= 1) {
        const uint32_t n = __shfl_up_sync(0xffffffff, val, offset);
        if (lane >= offset) {
            val += n;
        }
    }
    return val;
}
} // namespace

__launch_bounds__(256) __global__
    void hvs_scan_reduce(const uint32_t *block_counts, uint32_t *chunk_totals,
                         unsigned total_blocks)
{
    __shared__ uint32_t s_warp_totals[8];
    const unsigned tid = threadIdx.x;
    const unsigned b = (unsigned)blockIdx.x * 256u + tid;
    const uint32_t val = (b < total_blocks) ? block_counts[b] : 0u;

    const int lane = (int)(tid & 31u);
    const int wid = (int)(tid >> 5u);

    const uint32_t warp_sum = warp_scan_inclusive(val);
    if (lane == 31) {
        s_warp_totals[wid] = warp_sum;
    }
    __syncthreads();

    if (wid == 0) {
        uint32_t wval = (lane < 8) ? s_warp_totals[lane] : 0u;
        wval = warp_scan_inclusive(wval);
        if (lane < 8) {
            s_warp_totals[lane] = wval;
        }
    }
    __syncthreads();

    if (tid == 0u) {
        chunk_totals[blockIdx.x] = s_warp_totals[7];
    }
}

__launch_bounds__(512) __global__
    void hvs_scan_prefix(const uint32_t *chunk_totals, uint32_t *chunk_offsets,
                         PsnrHvsHeader *header, unsigned num_chunks)
{
    if (threadIdx.x == 0u) {
        uint32_t running = 0u;
        for (unsigned c = 0; c < num_chunks; c++) {
            chunk_offsets[c] = running;
            running += chunk_totals[c];
        }
        header->plane_offsets[0] = 0u;
        header->plane_offsets[1] = 0u;
        header->plane_offsets[2] = 0u;
        header->total_terms = running;
    }
}

__launch_bounds__(256) __global__
    void hvs_compact(PsnrHvsKernelArgs args, const float *raw_terms, const uint64_t *block_masks,
                     const uint32_t *block_counts, const uint32_t *chunk_offsets,
                     float *packed_terms, PsnrHvsHeader *header)
{
    __shared__ uint32_t s_warp_totals[8];
    const unsigned tid = threadIdx.x;
    const unsigned b = (unsigned)blockIdx.x * 256u + tid;
    const uint32_t val = (b < args.total_blocks) ? block_counts[b] : 0u;

    const int lane = (int)(tid & 31u);
    const int wid = (int)(tid >> 5u);

    const uint32_t warp_sum = warp_scan_inclusive(val);
    if (lane == 31) {
        s_warp_totals[wid] = warp_sum;
    }
    __syncthreads();

    if (wid == 0) {
        uint32_t wval = (lane < 8) ? s_warp_totals[lane] : 0u;
        wval = warp_scan_inclusive(wval);
        if (lane < 8) {
            s_warp_totals[lane] = wval;
        }
    }
    __syncthreads();

    const uint32_t warp_prefix = (wid > 0) ? s_warp_totals[wid - 1] : 0u;
    const uint32_t intra_offset = warp_prefix + warp_sum - val;

    if (b < args.total_blocks) {
        const uint32_t global_base = chunk_offsets[blockIdx.x] + intra_offset;

#pragma unroll
        for (unsigned p = 0; p < PSNR_HVS_NUM_PLANES; p++) {
            if (p < args.n_planes && b == args.plane[p].first_block) {
                header->plane_offsets[p] = global_base;
            }
        }

        uint64_t mask = block_masks[b];
        if (mask != 0ULL) {
            // SAFETY: raw_terms has PSNR_HVS_TERMS floats per block, packed_terms capacity >= total_terms.
            const float *src = raw_terms + (size_t)b * PSNR_HVS_TERMS;
            float *dst = packed_terms + global_base;
            uint32_t out_idx = 0u;
            while (mask != 0ULL) {
                const int idx = __ffsll((long long)mask) - 1;
                dst[out_idx++] = src[idx];
                mask &= mask - 1ULL;
            }
        }
    }
}

} /* extern "C" */
