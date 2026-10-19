/**
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-2-Clause
 *
 *  HIP kernel for the psnr_hvs feature extractor: the ADR-1369 kernel of
 *  `core/src/feature/sycl/integer_psnr_hvs_sycl.cpp`, ported to HIP, with the
 *  exact terms of ADR-1397 (ADR-1401).
 *
 *  One dispatch covers every active plane of a frame from the raw samples
 *  (uint8_t at 8 bpc, uint16_t above). Two work-items per 8x8 block (step 7):
 *  2k takes block k of the reference, 2k + 1 the same block of the distorted
 *  picture. Each loads its block into its own LDS slot, takes the variance
 *  ratio of the untransformed samples, runs the integer DCT in place and
 *  forms its masking threshold; after one barrier the reference work-item
 *  reads its partner's threshold and coefficients and stores the block's 64
 *  masked coefficient errors.
 *
 *  ADR-1397: the kernel reproduces calc_psnrhvs()
 *  (third_party/xiph/psnr_hvs.c) operation for operation and stores every
 *  term it adds to its running sum, in its row-major order, instead of a
 *  per-block sum. The host adds them into one running float in the CPU's
 *  order (vmaf_psnr_hvs_plane_score), so the scores are the CPU's bit for
 *  bit. Three things carry that and must stay:
 *    - the masking table is the CPU's: (csf * 0.3885746225901003)^2 taken
 *      in double and stored as float (hvs_mask_value, evaluated at compile
 *      time);
 *    - the masking threshold takes the float product of the CPU's
 *      statement and a double square root before it is stored as float
 *      (hvs_threshold, ADR-1488);
 *    - the module is built with -ffp-contract=off and correctly rounded
 *      fp32 division (hip_cu_extra_flags in core/src/meson.build), because
 *      the CPU build never contracts a multiply and an add.
 */

#include <hip/hip_runtime.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "hip/integer_psnr_hvs_hip.h"

namespace
{

constexpr int kBlockDim = 8;
constexpr int kBlockArea = kBlockDim * kBlockDim;
constexpr unsigned kStep = 7u;
/* LDS words per work-item: one 8x8 block plus one pad word, so the
 * work-items of a wavefront index distinct banks for the same coefficient. */
constexpr unsigned kLaneStride = kBlockArea + 1u;

/* Contrast-sensitivity tables (csf_y / csf_cb420 / csf_cr420 of
 * third_party/xiph/psnr_hvs.c) and the masking tables calc_psnrhvs() derives
 * from them. */
struct HvsTables {
    float csf[PSNR_HVS_HIP_NUM_PLANES][kBlockArea];
    float mask[PSNR_HVS_HIP_NUM_PLANES][kBlockArea];
};

/* calc_psnrhvs(): mask = (csf * 0.3885746225901003) * (csf * 0.3885746225901003),
 * a double product stored as float. */
constexpr float hvs_mask_value(float csf)
{
    const double scaled = (double)csf * 0.3885746225901003;
    return (float)(scaled * scaled);
}

constexpr HvsTables hvs_make_tables()
{
    HvsTables tables = {
        {/* Y */
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
          0.478717061273f, 0.393021669543f, 0.330555063063f, 0.285345396658f}},
        {}};
    for (int plane = 0; plane < PSNR_HVS_HIP_NUM_PLANES; plane++) {
        for (int index = 0; index < kBlockArea; index++) {
            tables.mask[plane][index] = hvs_mask_value(tables.csf[plane][index]);
        }
    }
    return tables;
}

/* Constant-initialised: hipcc emits both tables as static data. */
__device__ constexpr HvsTables HVS_TABLES = hvs_make_tables();

/* Round-toward-zero right shift — matches OD_UNBIASED_RSHIFT32
 * macro in xiph/psnr_hvs.c.  Identical to the CUDA twin. */
__device__ inline int od_ashr(int value, unsigned shift)
{
    /* Arithmetic right shift without a signed operand: a negative value gets
     * its vacated high bits set. `shift` is 1..15. */
    const uint32_t bits = (uint32_t)value;
    const uint32_t sign_fill = (value < 0) ? ~(~0u >> shift) : 0u;
    return (int)((bits >> shift) | sign_fill);
}

__device__ inline int od_dct_rshift(int a, unsigned b)
{
    return od_ashr((int)(((unsigned int)a >> (32u - b)) + (unsigned int)a), b);
}

/* Forward 8-point DCT — port of od_bin_fdct8 from
 * libvmaf/src/feature/third_party/xiph/psnr_hvs.c:72.
 * Identical to the CUDA twin. */
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
    int t1h;
    int t4h;
    int t6h;
    t1 = t0 - t1;
    t1h = od_dct_rshift(t1, 1u);
    t0 -= t1h;
    t4 += t5;
    t4h = od_dct_rshift(t4, 1u);
    t5 -= t4h;
    t3 = t2 - t3;
    t2 -= od_dct_rshift(t3, 1u);
    t6 += t7;
    t6h = od_dct_rshift(t6, 1u);
    t7 = t6h - t7;
    t0 += t6h;
    t6 = t0 - t6;
    t2 = t4h - t2;
    t4 = t2 - t4;
    t0 -= od_ashr(t4 * 13573 + 16384, 15u);
    t4 += od_ashr(t0 * 11585 + 8192, 14u);
    t0 -= od_ashr(t4 * 13573 + 16384, 15u);
    t6 -= od_ashr(t2 * 21895 + 16384, 15u);
    t2 += od_ashr(t6 * 15137 + 8192, 14u);
    t6 -= od_ashr(t2 * 21895 + 16384, 15u);
    t3 += od_ashr(t5 * 19195 + 16384, 15u);
    t5 += od_ashr(t3 * 11585 + 8192, 14u);
    t3 -= od_ashr(t5 * 7489 + 4096, 13u);
    t7 = od_dct_rshift(t5, 1u) - t7;
    t5 -= t7;
    t3 = t1h - t3;
    t1 -= t3;
    t7 += od_ashr(t1 * 3227 + 16384, 15u);
    t1 -= od_ashr(t7 * 6393 + 16384, 15u);
    t7 += od_ashr(t1 * 3227 + 16384, 15u);
    t5 += od_ashr(t3 * 2485 + 4096, 13u);
    t3 -= od_ashr(t5 * 18205 + 16384, 15u);
    t5 += od_ashr(t3 * 2485 + 4096, 13u);
    y0 = t0;
    y1 = t1;
    y2 = t2;
    y3 = t3;
    y4 = t4;
    y5 = t5;
    y6 = t6;
    y7 = t7;
}

/* Pass 1 of od_bin_fdct8x8() for column i, written back over column i: the
 * CPU stores that transform as row i of its scratch, so after the eight
 * columns the block holds the scratch transposed. */
__device__ void hvs_fdct8_column(int *blk, int i)
{
    int y0;
    int y1;
    int y2;
    int y3;
    int y4;
    int y5;
    int y6;
    int y7;
    od_bin_fdct8(y0, y1, y2, y3, y4, y5, y6, y7, blk[i], blk[kBlockDim + i],
                 blk[(2 * kBlockDim) + i], blk[(3 * kBlockDim) + i], blk[(4 * kBlockDim) + i],
                 blk[(5 * kBlockDim) + i], blk[(6 * kBlockDim) + i], blk[(7 * kBlockDim) + i]);
    blk[i] = y0;
    blk[kBlockDim + i] = y1;
    blk[(2 * kBlockDim) + i] = y2;
    blk[(3 * kBlockDim) + i] = y3;
    blk[(4 * kBlockDim) + i] = y4;
    blk[(5 * kBlockDim) + i] = y5;
    blk[(6 * kBlockDim) + i] = y6;
    blk[(7 * kBlockDim) + i] = y7;
}

/* Pass 2 for index i: the CPU transforms column i of its scratch into row i
 * of the output; that column is row i of the transposed block, so the
 * transform reads row i and writes it back in place. Integer arithmetic:
 * neither the layout nor the order changes a coefficient. */
__device__ void hvs_fdct8_row(int *blk, int i)
{
    int *row = blk + ((ptrdiff_t)i * kBlockDim);
    int y0;
    int y1;
    int y2;
    int y3;
    int y4;
    int y5;
    int y6;
    int y7;
    od_bin_fdct8(y0, y1, y2, y3, y4, y5, y6, y7, row[0], row[1], row[2], row[3], row[4], row[5],
                 row[6], row[7]);
    row[0] = y0;
    row[1] = y1;
    row[2] = y2;
    row[3] = y3;
    row[4] = y4;
    row[5] = y5;
    row[6] = y6;
    row[7] = y7;
}

/* od_bin_fdct8x8() of one block, in place. */
__device__ void hvs_fdct8x8(int *blk)
{
    for (int i = 0; i < kBlockDim; i++) {
        hvs_fdct8_column(blk, i);
    }
    for (int i = 0; i < kBlockDim; i++) {
        hvs_fdct8_row(blk, i);
    }
}

/* This work-item's block: its plane, the image it reads, and where. */
struct HvsLaneBlock {
    const void *src;
    size_t origin;
    unsigned width;
    unsigned block;
    int plane;
};

__device__ HvsLaneBlock hvs_locate(const PsnrHvsHipKernelArgs &args, unsigned block, bool is_dist)
{
    int plane = 0;
    for (int p = 1; p < PSNR_HVS_HIP_NUM_PLANES; p++) {
        if ((unsigned)p < args.n_planes && block >= args.plane[p].first_block) {
            plane = p;
        }
    }
    const PsnrHvsHipPlaneArgs &geometry = args.plane[plane];
    const unsigned in_plane = block - geometry.first_block;
    const size_t origin_x = (size_t)(in_plane % geometry.blocks_x) * kStep;
    const size_t origin_y = (size_t)(in_plane / geometry.blocks_x) * kStep;
    HvsLaneBlock lane;
    lane.src = is_dist ? geometry.dist : geometry.ref;
    lane.origin = (origin_y * geometry.width) + origin_x;
    lane.width = geometry.width;
    lane.block = block;
    lane.plane = plane;
    return lane;
}

/* The CPU reads the raw integer samples (calc_psnrhvs()), so does this. */
__device__ void hvs_load_block(int *blk, const HvsLaneBlock &lane, bool wide)
{
    for (int row = 0; row < kBlockDim; row++) {
        const size_t source = lane.origin + ((size_t)row * lane.width);
        for (int col = 0; col < kBlockDim; col++) {
            blk[(row * kBlockDim) + col] =
                wide ? (int)static_cast<const uint16_t *>(lane.src)[source + col] :
                       (int)static_cast<const uint8_t *>(lane.src)[source + col];
        }
    }
}

/* Variance ratio of the untransformed samples, as calc_psnrhvs() takes it
 * before its DCT; the previous kernel's expressions and summation order. */
__device__ float hvs_variance_ratio(const int *blk)
{
    float means[4] = {0.f, 0.f, 0.f, 0.f};
    float gmean = 0.f;
    for (unsigned i = 0; i < 8u; i++) {
        for (unsigned j = 0; j < 8u; j++) {
            const unsigned sub = ((i & 12u) >> 2u) + ((j & 12u) >> 1u);
            gmean += (float)blk[i * 8 + j];
            means[sub] += (float)blk[i * 8 + j];
        }
    }
    gmean /= 64.f;
    for (float &mean : means) {
        mean /= 16.f;
    }
    float vars[4] = {0.f, 0.f, 0.f, 0.f};
    float gvar = 0.f;
    for (unsigned i = 0; i < 8u; i++) {
        for (unsigned j = 0; j < 8u; j++) {
            const unsigned sub = ((i & 12u) >> 2u) + ((j & 12u) >> 1u);
            const float ds = (float)blk[i * 8 + j] - gmean;
            gvar += ds * ds;
            const float qs = (float)blk[i * 8 + j] - means[sub];
            vars[sub] += qs * qs;
        }
    }
    gvar *= 1.f / 63.f * 64.f;
    for (float &var : vars)
        var *= 1.f / 15.f * 16.f;
    if (gvar > 0.f)
        gvar = (vars[0] + vars[1] + vars[2] + vars[3]) / gvar;
    return gvar;
}

/* Masking energy of the transformed block, DC skipped. */
__device__ float hvs_mask_energy(const int *blk, int plane)
{
    float mc = 0.f;
    for (int i = 0; i < 8; i++) {
        const int j0 = (i == 0) ? 1 : 0;
        for (int j = j0; j < 8; j++) {
            const int sq = blk[i * 8 + j] * blk[i * 8 + j];
            mc += (float)sq * HVS_TABLES.mask[plane][i * 8 + j];
        }
    }
    return mc;
}

/* calc_psnrhvs(): s_mask = sqrt(s_mask * s_gvar) / 32.f, stored as float
 * (ADR-1488). The product is a float product there, so it is one here (a
 * plain operator rounds once under the strict FP list of this kernel); the
 * square root is double there, so it is here. */
__device__ float hvs_threshold(float energy, float ratio)
{
    const float product = energy * ratio;
    return (float)(sqrt((double)product) / 32.0);
}

/* The 64 values calc_psnrhvs() adds to its running sum for one block, in its
 * order (row-major). */
__device__ uint64_t hvs_store_terms(float *terms, const int *ref, const int *dist, float threshold,
                                    int plane)
{
    uint64_t mask = 0ULL;
    for (unsigned i = 0; i < 8u; i++) {
        for (unsigned j = 0; j < 8u; j++) {
            const unsigned index = (i * 8u) + j;
            const float csf = HVS_TABLES.csf[plane][index];
            float error = (float)abs(ref[index] - dist[index]);
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

extern "C" {

/* 1D grid of PSNR_HVS_HIP_WG work-items: 2 * total_blocks active items.
 * Work-items past the last block run on block 0 so every item reaches the
 * barrier, and never store. */
__global__ __launch_bounds__(PSNR_HVS_HIP_WG) void psnr_hvs_hip(PsnrHvsHipKernelArgs args)
{
    __shared__ int s_blocks[PSNR_HVS_HIP_WG * kLaneStride];
    __shared__ float s_threshold[PSNR_HVS_HIP_WG];

    const unsigned local = threadIdx.x;
    const size_t id = ((size_t)blockIdx.x * PSNR_HVS_HIP_WG) + local;
    const bool active = id < 2u * (size_t)args.total_blocks;
    const bool is_dist = (id & 1u) != 0u;
    const HvsLaneBlock lane = hvs_locate(args, active ? (unsigned)(id >> 1u) : 0u, is_dist);
    int *blk = s_blocks + ((size_t)local * kLaneStride);
    hvs_load_block(blk, lane, args.wide != 0u);
    const float ratio = hvs_variance_ratio(blk);
    hvs_fdct8x8(blk);
    s_threshold[local] = hvs_threshold(hvs_mask_energy(blk, lane.plane), ratio);
    __syncthreads();
    if (!active || is_dist) {
        return;
    }
    /* calc_psnrhvs(): if (d_mask > s_mask) s_mask = d_mask. */
    float threshold = s_threshold[local];
    const float dist_threshold = s_threshold[local + 1u];
    if (dist_threshold > threshold) {
        threshold = dist_threshold;
    }
    // SAFETY: args.terms holds PSNR_HVS_HIP_TERMS floats per block and
    // lane.block < args.total_blocks on an active work-item.
    const uint64_t mask = hvs_store_terms(args.terms + ((size_t)lane.block * PSNR_HVS_HIP_TERMS),
                                          blk, blk + kLaneStride, threshold, lane.plane);
    if (args.block_masks != nullptr) {
        args.block_masks[lane.block] = mask;
    }
    if (args.block_counts != nullptr) {
        args.block_counts[lane.block] = (uint32_t)__builtin_popcountll(mask);
    }
}

__global__ __launch_bounds__(256) void hvs_scan_reduce_hip(const uint32_t *block_counts,
                                                           uint32_t *chunk_totals,
                                                           unsigned total_blocks)
{
    __shared__ uint32_t s_data[256];
    const unsigned tid = threadIdx.x;
    const unsigned chunk = blockIdx.x;
    const unsigned b = chunk * 256u + tid;
    const uint32_t val = (b < total_blocks) ? block_counts[b] : 0u;
    s_data[tid] = val;
    __syncthreads();

    for (uint32_t offset = 1u; offset < 256u; offset *= 2u) {
        uint32_t n = 0u;
        if (tid >= offset) {
            n = s_data[tid - offset];
        }
        __syncthreads();
        s_data[tid] += n;
        __syncthreads();
    }

    if (tid == 255u) {
        chunk_totals[chunk] = s_data[255u];
    }
}

__global__ __launch_bounds__(1) void hvs_scan_prefix_hip(const uint32_t *chunk_totals,
                                                         uint32_t *chunk_offsets,
                                                         struct PsnrHvsHipHeader *header,
                                                         unsigned num_chunks)
{
    /* Every chunk: a cap here left the offsets of the chunks past it unset,
     * and hvs_compact_hip wrote their terms out of bounds (more than 32768
     * chunks: 16384x8640 4:4:4 and larger, 256,779 at the 32768 cap). */
    uint32_t running = 0u;
    for (unsigned c = 0u; c < num_chunks; c++) {
        chunk_offsets[c] = running;
        running += chunk_totals[c];
    }
    header->plane_offsets[0] = 0u;
    header->plane_offsets[1] = 0u;
    header->plane_offsets[2] = 0u;
    header->total_terms = running;
}

__global__ __launch_bounds__(256) void hvs_compact_hip(
    struct PsnrHvsHipKernelArgs args, const float *raw_terms, const uint64_t *block_masks,
    const uint32_t *block_counts, const uint32_t *chunk_offsets, float *packed_terms,
    struct PsnrHvsHipHeader *header)
{
    __shared__ uint32_t s_data[256];
    const unsigned tid = threadIdx.x;
    const unsigned chunk = blockIdx.x;
    const unsigned b = chunk * 256u + tid;
    const uint32_t count = (b < args.total_blocks) ? block_counts[b] : 0u;
    s_data[tid] = count;
    __syncthreads();

    for (uint32_t offset = 1u; offset < 256u; offset *= 2u) {
        uint32_t n = 0u;
        if (tid >= offset) {
            n = s_data[tid - offset];
        }
        __syncthreads();
        s_data[tid] += n;
        __syncthreads();
    }

    const uint32_t intra_offset = s_data[tid] - count;
    const uint32_t global_base = chunk_offsets[chunk] + intra_offset;

    if (b < args.total_blocks) {
        if (0u < args.n_planes && b == args.plane[0].first_block) {
            header->plane_offsets[0] = global_base;
        }
        if (1u < args.n_planes && b == args.plane[1].first_block) {
            header->plane_offsets[1] = global_base;
        }
        if (2u < args.n_planes && b == args.plane[2].first_block) {
            header->plane_offsets[2] = global_base;
        }

        uint64_t mask = block_masks[b];
        if (mask != 0ULL) {
            // SAFETY: raw_terms holds PSNR_HVS_HIP_TERMS floats per block; packed_terms capacity >= total_terms.
            const float *src = raw_terms + ((size_t)b * PSNR_HVS_HIP_TERMS);
            float *dst = packed_terms + global_base;
            uint32_t out_idx = 0u;
            for (int bit = 0; bit < 64 && mask != 0ULL; bit++) {
                const int idx = __builtin_ctzll(mask);
                dst[out_idx++] = src[idx];
                mask &= mask - 1ULL;
            }
        }
    }
}

} /* extern "C" */
