/**
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-2-Clause
 *
 *  CUDA compute kernel for the psnr_hvs feature extractor
 *  (T7-23 / batch 2 part 3b / ADR-0188 / ADR-0191). Mirrors
 *  the Vulkan psnr_hvs.comp byte-for-byte modulo language
 *  differences. One CUDA kernel, one block per output 8×8
 *  block (step=7), 64 threads/block.
 *
 *  Cooperative load + thread-0-serial float reductions matching CPU's
 *  linear i,j summation order (same precision strategy as the
 *  Vulkan kernel: lock per-block bit-order to CPU's calc_psnrhvs
 *  computation pattern). The two integer DCT passes are row/column
 *  parallel across the first eight CUDA threads; masking and float
 *  accumulation stay on thread 0 to preserve the established
 *  reduction order.
 */

#include "cuda_helper.cuh"
#include "cuda/integer_psnr_hvs_cuda.h"
#include "common.h"

#define BLOCK_DIM 8
#define BLOCK_SIZE (BLOCK_DIM * BLOCK_DIM)

/* Per-plane CSF tables — same constants as csf_y / csf_cb420 /
 * csf_cr420 in third_party/xiph/psnr_hvs.c. */
__device__ static const float CSF_TABLES[3][64] = {
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
 * macro in xiph/psnr_hvs.c. C/C++ signed `>>` of negatives is
 * implementation-defined, typically arithmetic shift (rounds
 * toward -inf). Adding the sign bit shifted to the low position
 * before the shift biases negatives toward zero. */
__device__ static inline int od_dct_rshift(int a, int b)
{
    return (int)(((unsigned int)a >> (32 - b)) + (unsigned int)a) >> b;
}

/* Forward 8-point DCT — port of od_bin_fdct8 from
 * libvmaf/src/feature/third_party/xiph/psnr_hvs.c:72. */
__device__ static void od_bin_fdct8(int &y0, int &y1, int &y2, int &y3, int &y4, int &y5, int &y6,
                                    int &y7, int x0, int x1, int x2, int x3, int x4, int x5, int x6,
                                    int x7)
{
    int t0 = x0;
    int t4 = x1;
    int t2 = x2;
    int t6 = x3;
    int t7 = x4;
    int t3 = x5;
    int t5 = x6;
    int t1 = x7;
    int t1h, t4h, t6h;
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

__device__ static void od_bin_fdct8x8_parallel(int blk[64], int z[64], unsigned lane)
{
    /* Pass 1: read input column i, write z[8i + 0..7] = column DCT. */
    if (lane < 8u) {
        int y0, y1, y2, y3, y4, y5, y6, y7;
        od_bin_fdct8(y0, y1, y2, y3, y4, y5, y6, y7, blk[0 * 8 + lane], blk[1 * 8 + lane],
                     blk[2 * 8 + lane], blk[3 * 8 + lane], blk[4 * 8 + lane], blk[5 * 8 + lane],
                     blk[6 * 8 + lane], blk[7 * 8 + lane]);
        z[lane * 8 + 0] = y0;
        z[lane * 8 + 1] = y1;
        z[lane * 8 + 2] = y2;
        z[lane * 8 + 3] = y3;
        z[lane * 8 + 4] = y4;
        z[lane * 8 + 5] = y5;
        z[lane * 8 + 6] = y6;
        z[lane * 8 + 7] = y7;
    }
    __syncthreads();

    /* Pass 2: read column i of z, write blk[8i + 0..7] = 2-D DCT row. */
    if (lane < 8u) {
        int y0, y1, y2, y3, y4, y5, y6, y7;
        od_bin_fdct8(y0, y1, y2, y3, y4, y5, y6, y7, z[0 * 8 + lane], z[1 * 8 + lane],
                     z[2 * 8 + lane], z[3 * 8 + lane], z[4 * 8 + lane], z[5 * 8 + lane],
                     z[6 * 8 + lane], z[7 * 8 + lane]);
        blk[lane * 8 + 0] = y0;
        blk[lane * 8 + 1] = y1;
        blk[lane * 8 + 2] = y2;
        blk[lane * 8 + 3] = y3;
        blk[lane * 8 + 4] = y4;
        blk[lane * 8 + 5] = y5;
        blk[lane * 8 + 6] = y6;
        blk[lane * 8 + 7] = y7;
    }
    __syncthreads();
}

__device__ static inline int sample_to_int(float v, int bpc)
{
    /* picture_copy normalises uint sample → float in [0, 255]
     * (8-bit: scale=1; 10-bit: /4; 12-bit: /16). Reverse here. */
    if (bpc == 8)
        return (int)(v + 0.5f);
    if (bpc == 10)
        return (int)(v * 4.0f + 0.5f);
    return (int)(v * 16.0f + 0.5f);
}

__device__ static void compute_masking_variances(const int ref[64], const int dist[64],
                                                 float *ref_gvar, float *dist_gvar)
{
    float ref_means[4] = {0.f, 0.f, 0.f, 0.f};
    float dist_means[4] = {0.f, 0.f, 0.f, 0.f};
    float ref_vars[4] = {0.f, 0.f, 0.f, 0.f};
    float dist_vars[4] = {0.f, 0.f, 0.f, 0.f};
    float ref_gmean = 0.f, dist_gmean = 0.f;
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            const int sub = ((i & 12) >> 2) + ((j & 12) >> 1);
            ref_gmean += (float)ref[i * 8 + j];
            dist_gmean += (float)dist[i * 8 + j];
            ref_means[sub] += (float)ref[i * 8 + j];
            dist_means[sub] += (float)dist[i * 8 + j];
        }
    }
    ref_gmean /= 64.f;
    dist_gmean /= 64.f;
    for (int i = 0; i < 4; i++)
        ref_means[i] /= 16.f;
    for (int i = 0; i < 4; i++)
        dist_means[i] /= 16.f;
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            const int sub = ((i & 12) >> 2) + ((j & 12) >> 1);
            const float ref_delta = (float)ref[i * 8 + j] - ref_gmean;
            const float dist_delta = (float)dist[i * 8 + j] - dist_gmean;
            *ref_gvar += ref_delta * ref_delta;
            *dist_gvar += dist_delta * dist_delta;
            const float ref_quad = (float)ref[i * 8 + j] - ref_means[sub];
            const float dist_quad = (float)dist[i * 8 + j] - dist_means[sub];
            ref_vars[sub] += ref_quad * ref_quad;
            dist_vars[sub] += dist_quad * dist_quad;
        }
    }
    *ref_gvar *= 1.f / 63.f * 64.f;
    *dist_gvar *= 1.f / 63.f * 64.f;
    for (int i = 0; i < 4; i++)
        ref_vars[i] *= 1.f / 15.f * 16.f;
    for (int i = 0; i < 4; i++)
        dist_vars[i] *= 1.f / 15.f * 16.f;
    if (*ref_gvar > 0.f)
        *ref_gvar = (ref_vars[0] + ref_vars[1] + ref_vars[2] + ref_vars[3]) / *ref_gvar;
    if (*dist_gvar > 0.f)
        *dist_gvar = (dist_vars[0] + dist_vars[1] + dist_vars[2] + dist_vars[3]) / *dist_gvar;
}

__device__ static void build_psnr_hvs_mask(float mask[64], int plane)
{
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            const float weighted = CSF_TABLES[plane][i * 8 + j] * 0.3885746225901003f;
            mask[i * 8 + j] = weighted * weighted;
        }
    }
}

__device__ static float masking_energy(const int dct[64], const float mask[64])
{
    float energy = 0.f;
    for (int i = 0; i < 8; i++) {
        const int first_column = (i == 0) ? 1 : 0;
        for (int j = first_column; j < 8; j++) {
            const int square = dct[i * 8 + j] * dct[i * 8 + j];
            energy += (float)square * mask[i * 8 + j];
        }
    }
    return energy;
}

__device__ static float masked_error(const int ref_dct[64], const int dist_dct[64],
                                     const float mask[64], float threshold, int plane)
{
    float result = 0.f;
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            const float csf = CSF_TABLES[plane][i * 8 + j];
            float error = fabsf((float)ref_dct[i * 8 + j] - (float)dist_dct[i * 8 + j]);
            if (i != 0 || j != 0) {
                const float masked_threshold = threshold / mask[i * 8 + j];
                error = error < masked_threshold ? 0.f : error - masked_threshold;
            }
            result += (error * csf) * (error * csf);
        }
    }
    return result;
}

__device__ static void load_psnr_hvs_sample(VmafCudaBuffer ref_in, VmafCudaBuffer dist_in,
                                            int ref[64], int dist[64], int ref_dct[64],
                                            int dist_dct[64], unsigned local_idx, unsigned x,
                                            unsigned y, unsigned width, bool valid, int bpc)
{
    const float *__restrict__ ref_buf = reinterpret_cast<const float *>(ref_in.data);
    const float *__restrict__ dist_buf = reinterpret_cast<const float *>(dist_in.data);
    int ref_sample = 0;
    int dist_sample = 0;
    if (valid) {
        const unsigned source_index = y * width + x;
        ref_sample = sample_to_int(__ldg(&ref_buf[source_index]), bpc);
        dist_sample = sample_to_int(__ldg(&dist_buf[source_index]), bpc);
    }
    ref[local_idx] = ref_sample;
    dist[local_idx] = dist_sample;
    ref_dct[local_idx] = ref_sample;
    dist_dct[local_idx] = dist_sample;
}

/* psnr_hvs kernel: one CUDA block per output 8×8 image block.
 * Cooperative load (64 threads), then thread 0 runs the float
 * means / variances in CPU's exact i,j summation order. The first
 * eight threads run the integer DCT passes in parallel; thread 0
 * resumes for the masking and final float reduction so the
 * established CUDA/Vulkan numeric contract stays unchanged.
 *
 * __launch_bounds__(64): hints nvcc to budget registers for
 * 64-thread blocks (8×8); per ADR-0764 / ADR-0754 precedent. */
extern "C" __launch_bounds__(64) __global__
    void psnr_hvs(VmafCudaBuffer ref_in, VmafCudaBuffer dist_in, VmafCudaBuffer partials_out,
                  unsigned width, unsigned height, unsigned num_blocks_x, unsigned num_blocks_y,
                  int plane, int bpc)
{
    __shared__ int s_ref[64];
    __shared__ int s_dist[64];
    __shared__ int dct_s[64];
    __shared__ int dct_d[64];
    __shared__ int z_s[64];
    __shared__ int z_d[64];

    const unsigned blk_x = blockIdx.x;
    const unsigned blk_y = blockIdx.y;
    const unsigned lx = threadIdx.x;
    const unsigned ly = threadIdx.y;
    const unsigned local_idx = ly * 8u + lx;

    const unsigned x0 = blk_x * 7u;
    const unsigned y0 = blk_y * 7u;
    const bool valid_block =
        (blk_x < num_blocks_x && blk_y < num_blocks_y && x0 + 7u < width && y0 + 7u < height);

    load_psnr_hvs_sample(ref_in, dist_in, s_ref, s_dist, dct_s, dct_d, local_idx, x0 + lx, y0 + ly,
                         width, valid_block, bpc);
    __syncthreads();

    float s_gvar = 0.f, d_gvar = 0.f;
    if (local_idx == 0u)
        compute_masking_variances(s_ref, s_dist, &s_gvar, &d_gvar);

    /* Integer DCT in place, parallel across the first eight threads. */
    od_bin_fdct8x8_parallel(dct_s, z_s, local_idx);
    od_bin_fdct8x8_parallel(dct_d, z_d, local_idx);

    if (local_idx != 0u)
        return;

    float mask[64];
    build_psnr_hvs_mask(mask, plane);
    const float s_mc = masking_energy(dct_s, mask);
    const float d_mc = masking_energy(dct_d, mask);
    float sm = sqrtf(s_mc * s_gvar) / 32.f;
    const float dm = sqrtf(d_mc * d_gvar) / 32.f;
    if (dm > sm)
        sm = dm;
    const float thresh = sm;

    float ret = masked_error(dct_s, dct_d, mask, thresh, plane);
    if (!valid_block)
        ret = 0.f;

    const unsigned slot = blk_y * num_blocks_x + blk_x;
    reinterpret_cast<float *>(partials_out.data)[slot] = ret;
}
