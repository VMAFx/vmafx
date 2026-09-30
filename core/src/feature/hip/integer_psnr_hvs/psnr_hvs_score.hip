/**
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-2-Clause
 *
 *  HIP kernel for the psnr_hvs feature extractor: the ADR-1369 kernel of
 *  `core/src/feature/sycl/integer_psnr_hvs_sycl.cpp`, ported to HIP
 *  (ADR-ADRNUM).
 *
 *  One dispatch scores every active plane of a frame from the raw samples
 *  (uint8_t at 8 bpc, uint16_t above). Two work-items per 8x8 block (step 7):
 *  2k takes block k of the reference, 2k + 1 the same block of the distorted
 *  picture. Each loads its block into its own LDS slot, takes the variance
 *  ratio of the untransformed samples, runs the integer DCT in place and the
 *  masking energy; after one barrier the reference work-item reads its
 *  partner's statistics and coefficients and writes the block's masked error.
 *  The host sums the block errors of each plane in block order.
 *
 *  Every per-block float expression is the one the previous kernel (one
 *  64-thread work-group per block, thread 0 serial) evaluated, in the same
 *  order and spelled the same way, so this module keeps its default
 *  contraction and the block errors are bit-identical to that kernel's. The
 *  previous kernel took float samples that the host had normalised and
 *  converted them back with a 8 / 10 / other-bpc switch that scaled 9- and
 *  11-bit input by 16 instead of 2 and 8; reading the raw samples removes
 *  that conversion.
 */

#include <hip/hip_runtime.h>

#include <cstddef>
#include <cstdint>

#include "hip/integer_psnr_hvs_hip.h"

namespace
{

constexpr int kBlockDim = 8;
constexpr int kBlockArea = kBlockDim * kBlockDim;
constexpr unsigned kStep = 7u;
/* LDS words per work-item: one 8x8 block plus one pad word, so the
 * work-items of a wavefront index distinct banks for the same coefficient. */
constexpr unsigned kLaneStride = kBlockArea + 1u;

/* Per-plane CSF tables — identical to the CUDA twin (same constants as
 * csf_y / csf_cb420 / csf_cr420 in third_party/xiph/psnr_hvs.c). */
__device__ const float CSF_TABLES[3][64] = {
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

/* Round-toward-zero right shift — matches OD_UNBIASED_RSHIFT32
 * macro in xiph/psnr_hvs.c.  Identical to the CUDA twin. */
__device__ inline int od_dct_rshift(int a, int b)
{
    return (int)(((unsigned int)a >> (32 - b)) + (unsigned int)a) >> b;
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
    t1h = od_dct_rshift(t1, 1);
    t0 -= t1h;
    t4 += t5;
    t4h = od_dct_rshift(t4, 1);
    t5 -= t4h;
    t3 = t2 - t3;
    t2 -= od_dct_rshift(t3, 1);
    t6 += t7;
    t6h = od_dct_rshift(t6, 1);
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
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            const int sub = ((i & 12) >> 2) + ((j & 12) >> 1);
            gmean += (float)blk[i * 8 + j];
            means[sub] += (float)blk[i * 8 + j];
        }
    }
    gmean /= 64.f;
    for (float &mean : means)
        mean /= 16.f;
    float vars[4] = {0.f, 0.f, 0.f, 0.f};
    float gvar = 0.f;
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            const int sub = ((i & 12) >> 2) + ((j & 12) >> 1);
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

/* mask[i][j] of calc_psnrhvs(): the same two float multiplies. */
__device__ float hvs_mask_at(int plane, int index)
{
    const float c = CSF_TABLES[plane][index];
    const float m = c * 0.3885746225901003f;
    return m * m;
}

/* Masking energy of the transformed block, DC skipped. */
__device__ float hvs_mask_energy(const int *blk, int plane)
{
    float mc = 0.f;
    for (int i = 0; i < 8; i++) {
        const int j0 = (i == 0) ? 1 : 0;
        for (int j = j0; j < 8; j++) {
            const int sq = blk[i * 8 + j] * blk[i * 8 + j];
            mc += (float)sq * hvs_mask_at(plane, i * 8 + j);
        }
    }
    return mc;
}

/* Masked error of one block from both images' coefficients. */
__device__ float hvs_error(const int *ref, const int *dist, float thresh, int plane)
{
    float ret = 0.f;
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            const float c = CSF_TABLES[plane][i * 8 + j];
            float err = fabsf((float)ref[i * 8 + j] - (float)dist[i * 8 + j]);
            if (i != 0 || j != 0) {
                const float t = thresh / hvs_mask_at(plane, i * 8 + j);
                err = err < t ? 0.f : err - t;
            }
            ret += (err * c) * (err * c);
        }
    }
    return ret;
}

} // namespace

extern "C" {

/* 1D grid of PSNR_HVS_HIP_WG work-items: 2 * total_blocks active items.
 * Work-items past the last block run on block 0 so every item reaches the
 * barrier, and never store. */
__global__ __launch_bounds__(PSNR_HVS_HIP_WG) void psnr_hvs_hip(PsnrHvsHipKernelArgs args)
{
    __shared__ int s_blocks[PSNR_HVS_HIP_WG * kLaneStride];
    __shared__ float s_ratio[PSNR_HVS_HIP_WG];
    __shared__ float s_energy[PSNR_HVS_HIP_WG];

    const unsigned local = threadIdx.x;
    const size_t id = ((size_t)blockIdx.x * PSNR_HVS_HIP_WG) + local;
    const bool active = id < 2u * (size_t)args.total_blocks;
    const bool is_dist = (id & 1u) != 0u;
    const HvsLaneBlock lane = hvs_locate(args, active ? (unsigned)(id >> 1) : 0u, is_dist);
    int *blk = s_blocks + ((size_t)local * kLaneStride);
    hvs_load_block(blk, lane, args.wide != 0u);
    s_ratio[local] = hvs_variance_ratio(blk);
    hvs_fdct8x8(blk);
    s_energy[local] = hvs_mask_energy(blk, lane.plane);
    __syncthreads();
    if (!active || is_dist) {
        return;
    }
    const unsigned partner = local + 1u;
    float sm = sqrtf(s_energy[local] * s_ratio[local]) / 32.f;
    const float dm = sqrtf(s_energy[partner] * s_ratio[partner]) / 32.f;
    if (dm > sm)
        sm = dm;
    args.partials[lane.block] = hvs_error(blk, blk + kLaneStride, sm, lane.plane);
}

} /* extern "C" */
