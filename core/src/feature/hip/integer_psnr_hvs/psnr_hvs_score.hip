/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  HIP compute kernel for the psnr_hvs feature extractor.
 *  Direct port of the CUDA twin at
 *  `libvmaf/src/feature/cuda/integer_psnr_hvs/psnr_hvs_score.cu`.
 *
 *  HIP adaptations from CUDA:
 *  - `#include <hip/hip_runtime.h>` replaces `cuda_helper.cuh` / CUDA headers.
 *  - `__syncthreads()` is identical in HIP; `__shfl_down` is available.
 *  - `VmafCudaBuffer` is replaced by plain `float *` pointers — the host
 *    marshals tightly-pitched device buffers (same pattern as
 *    `integer_psnr/psnr_score.hip` and `float_psnr/float_psnr_score.hip`).
 *  - Kernel parameter list uses plain `float *` for ref/dist/partials
 *    instead of the `VmafCudaBuffer` wrapper struct.
 *  - The rest of the algorithm (CSF tables, od_bin_fdct8, means/variances,
 *    masked-error accumulation) is byte-for-byte identical to the CUDA twin
 *    to preserve the established CUDA/HIP numeric contract.
 *
 *  One HIP block per output 8x8 image block (step=7), 64 threads/block
 *  (8x8 2-D thread grid, same as CUDA). Cooperative load + thread-0-serial
 *  float reductions matching CPU's i,j summation order.
 */

#include <hip/hip_runtime.h>

#define BLOCK_DIM 8
#define BLOCK_SIZE (BLOCK_DIM * BLOCK_DIM)

extern "C" {

/* Per-plane CSF tables — identical to the CUDA twin (same constants as
 * csf_y / csf_cb420 / csf_cr420 in third_party/xiph/psnr_hvs.c). */
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
 * macro in xiph/psnr_hvs.c.  Identical to the CUDA twin. */
__device__ static inline int od_dct_rshift(int a, int b)
{
    return (int)(((unsigned int)a >> (32 - b)) + (unsigned int)a) >> b;
}

/* Forward 8-point DCT — port of od_bin_fdct8 from
 * libvmaf/src/feature/third_party/xiph/psnr_hvs.c:72.
 * Identical to the CUDA twin. */
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
    /* picture_copy normalises uint sample to float in [0, 255]
     * (8-bit: scale=1; 10-bit: /4; 12-bit: /16). Reverse here.
     * Identical to the CUDA twin. */
    if (bpc == 8)
        return (int)(v + 0.5f);
    if (bpc == 10)
        return (int)(v * 4.0f + 0.5f);
    return (int)(v * 16.0f + 0.5f);
}

/**
 * psnr_hvs kernel: one HIP block per output 8x8 image block.
 * Cooperative load (64 threads), then thread 0 runs float means /
 * variances in CPU's exact i,j summation order. The first eight
 * threads run the integer DCT passes in parallel; thread 0 resumes
 * for the masking and final float reduction.
 *
 * HIP adaptation: ref_in / dist_in / partials_out are plain float *
 * (tightly-pitched device buffers, no VmafCudaBuffer wrapper).
 * Everything else is identical to the CUDA twin.
 */
__global__ void psnr_hvs_hip(const float *__restrict__ ref_in,
                              const float *__restrict__ dist_in,
                              float *__restrict__ partials_out,
                              unsigned width,
                              unsigned height,
                              unsigned num_blocks_x,
                              unsigned num_blocks_y,
                              int plane,
                              int bpc)
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

    int my_ref = 0;
    int my_dist = 0;
    if (valid_block) {
        const unsigned sx = x0 + lx;
        const unsigned sy = y0 + ly;
        const unsigned src_idx = sy * width + sx;
        my_ref = sample_to_int(ref_in[src_idx], bpc);
        my_dist = sample_to_int(dist_in[src_idx], bpc);
    }
    s_ref[local_idx] = my_ref;
    s_dist[local_idx] = my_dist;
    dct_s[local_idx] = my_ref;
    dct_d[local_idx] = my_dist;
    __syncthreads();

    float s_means[4] = {0.f, 0.f, 0.f, 0.f};
    float d_means[4] = {0.f, 0.f, 0.f, 0.f};
    float s_vars[4] = {0.f, 0.f, 0.f, 0.f};
    float d_vars[4] = {0.f, 0.f, 0.f, 0.f};
    float s_gmean = 0.f, d_gmean = 0.f;
    float s_gvar = 0.f, d_gvar = 0.f;
    float s_mc = 0.f, d_mc = 0.f;

    if (local_idx == 0u) {
        /* Pass 1: means. */
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 8; j++) {
                const int sub = ((i & 12) >> 2) + ((j & 12) >> 1);
                s_gmean += (float)s_ref[i * 8 + j];
                d_gmean += (float)s_dist[i * 8 + j];
                s_means[sub] += (float)s_ref[i * 8 + j];
                d_means[sub] += (float)s_dist[i * 8 + j];
            }
        }
        s_gmean /= 64.f;
        d_gmean /= 64.f;
        for (int i = 0; i < 4; i++)
            s_means[i] /= 16.f;
        for (int i = 0; i < 4; i++)
            d_means[i] /= 16.f;

        /* Pass 2: variances. */
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 8; j++) {
                const int sub = ((i & 12) >> 2) + ((j & 12) >> 1);
                const float ds = (float)s_ref[i * 8 + j] - s_gmean;
                const float dd = (float)s_dist[i * 8 + j] - d_gmean;
                s_gvar += ds * ds;
                d_gvar += dd * dd;
                const float qs = (float)s_ref[i * 8 + j] - s_means[sub];
                const float qd = (float)s_dist[i * 8 + j] - d_means[sub];
                s_vars[sub] += qs * qs;
                d_vars[sub] += qd * qd;
            }
        }
        s_gvar *= 1.f / 63.f * 64.f;
        d_gvar *= 1.f / 63.f * 64.f;
        for (int i = 0; i < 4; i++)
            s_vars[i] *= 1.f / 15.f * 16.f;
        for (int i = 0; i < 4; i++)
            d_vars[i] *= 1.f / 15.f * 16.f;
        if (s_gvar > 0.f)
            s_gvar = (s_vars[0] + s_vars[1] + s_vars[2] + s_vars[3]) / s_gvar;
        if (d_gvar > 0.f)
            d_gvar = (d_vars[0] + d_vars[1] + d_vars[2] + d_vars[3]) / d_gvar;
    }

    /* Integer DCT in place, parallel across the first eight threads. */
    od_bin_fdct8x8_parallel(dct_s, z_s, local_idx);
    od_bin_fdct8x8_parallel(dct_d, z_d, local_idx);

    if (local_idx != 0u)
        return;

    /* Pass 3: per-coefficient mask*dct^2 accumulation, skipping DC. */
    float mask[64];
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            const float c = CSF_TABLES[plane][i * 8 + j];
            const float m = c * 0.3885746225901003f;
            mask[i * 8 + j] = m * m;
        }
    }
    for (int i = 0; i < 8; i++) {
        const int j0 = (i == 0) ? 1 : 0;
        for (int j = j0; j < 8; j++) {
            const int sq = dct_s[i * 8 + j] * dct_s[i * 8 + j];
            s_mc += (float)sq * mask[i * 8 + j];
        }
    }
    for (int i = 0; i < 8; i++) {
        const int j0 = (i == 0) ? 1 : 0;
        for (int j = j0; j < 8; j++) {
            const int sq = dct_d[i * 8 + j] * dct_d[i * 8 + j];
            d_mc += (float)sq * mask[i * 8 + j];
        }
    }
    float sm = sqrtf(s_mc * s_gvar) / 32.f;
    const float dm = sqrtf(d_mc * d_gvar) / 32.f;
    if (dm > sm)
        sm = dm;
    const float thresh = sm;

    /* Pass 4: per-coefficient masked-error contribution. */
    float ret = 0.f;
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            const float c = CSF_TABLES[plane][i * 8 + j];
            float err = fabsf((float)dct_s[i * 8 + j] - (float)dct_d[i * 8 + j]);
            if (i != 0 || j != 0) {
                const float t = thresh / mask[i * 8 + j];
                err = err < t ? 0.f : err - t;
            }
            ret += (err * c) * (err * c);
        }
    }
    if (!valid_block)
        ret = 0.f;

    const unsigned slot = blk_y * num_blocks_x + blk_x;
    partials_out[slot] = ret;
}

} /* extern "C" */
