/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Metal compute kernel for the psnr_hvs feature extractor — Metal twin
 *  of the CUDA reference
 *  libvmaf/src/feature/cuda/integer_psnr_hvs/psnr_hvs_score.cu and the
 *  CPU reference libvmaf/src/feature/third_party/xiph/psnr_hvs.c
 *  (feature name "psnr_hvs", emits psnr_hvs_y / psnr_hvs_cb /
 *  psnr_hvs_cr / psnr_hvs).
 *
 *  One threadgroup per output 8x8 image block (sliding window, step=7),
 *  64 threads/threadgroup (8x8). The structure mirrors the CUDA kernel
 *  byte-for-byte modulo language differences:
 *    - cooperative 64-thread load of the 8x8 ref/dist tile;
 *    - thread 0 computes float means / variances in the CPU's exact
 *      i,j summation order (locks the float reduction bit-order to the
 *      scalar calc_psnrhvs register semantics);
 *    - the integer od_bin_fdct8x8 runs row/column-parallel across the
 *      first eight threads (matches od_bin_fdct8x8 two-pass layout);
 *    - thread 0 resumes for the masking and final masked-error float
 *      accumulation, writing one float partial per block.
 *
 *  No 64-bit MSL atomics: every block writes a single float partial to
 *  partials_out[blk_y * num_blocks_x + blk_x]; the host sums the
 *  partials in a float register (matching CPU's `ret`) then divides by
 *  pixels and samplemax^2.
 *
 *  bpc handling: the 8bpc kernel reads raw uchar samples. The 16bpc
 *  kernel reads raw ushort samples WITHOUT any scaler division — the
 *  CPU reference (third_party/xiph/psnr_hvs.c) feeds the full
 *  10/12-bit integer pixel values into the DCT and divides the final
 *  score by samplemax^2 = ((1<<bpc)-1)^2. (This is a deliberate
 *  divergence from the CUDA twin, which normalises to an 8-bit-
 *  equivalent float in picture_copy and reverses it via sample_to_int;
 *  matching CPU directly is the parity target for this backend.)
 *
 *  Buffer bindings (same for 8bpc and 16bpc; sample width differs):
 *   [[buffer(0)]] ref       — const uchar *  (plane, byte-addressed)
 *   [[buffer(1)]] dis       — const uchar *
 *   [[buffer(2)]] partials  — float *  (num_blocks_x x num_blocks_y)
 *   [[buffer(3)]] csf       — const float * (64 CSF entries for the plane)
 *   [[buffer(4)]] dims      — uint4 (.x=width, .y=height,
 *                                    .z=num_blocks_x, .w=num_blocks_y)
 *   [[buffer(5)]] strides   — uint2 (.x=ref_row_bytes, .y=dis_row_bytes)
 */

#include <metal_stdlib>
using namespace metal;

/* ------------------------------------------------------------------ */
/*  Integer DCT helpers — direct port of od_bin_fdct8 /                */
/*  od_dct_rshift from third_party/xiph/psnr_hvs.c (via the CUDA twin) */
/* ------------------------------------------------------------------ */

/* Round-toward-zero right shift — matches OD_UNBIASED_RSHIFT32 in
 * xiph/psnr_hvs.c. Signed `>>` of negatives is arithmetic (rounds
 * toward -inf); adding the sign bit shifted to the low position before
 * the shift biases negatives toward zero. */
static inline int od_dct_rshift(int a, int b)
{
    return (int)(((uint)a >> (32 - b)) + (uint)a) >> b;
}

/* Forward 8-point DCT — port of od_bin_fdct8 from
 * third_party/xiph/psnr_hvs.c:73. */
static void od_bin_fdct8(thread int &y0, thread int &y1, thread int &y2, thread int &y3,
                         thread int &y4, thread int &y5, thread int &y6, thread int &y7,
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

/* Two-pass 8x8 DCT, parallel across the first eight lanes — mirrors
 * od_bin_fdct8x8_parallel in psnr_hvs_score.cu. `blk` holds the input
 * (overwritten with the 2-D DCT); `z` is scratch for the column pass. */
static void od_bin_fdct8x8_parallel(threadgroup int *blk, threadgroup int *z, uint lane)
{
    /* Pass 1: read input column `lane`, write z[lane*8 + 0..7] (column DCT). */
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
    threadgroup_barrier(mem_flags::mem_threadgroup);

    /* Pass 2: read column `lane` of z, write blk[lane*8 + 0..7] (2-D DCT row). */
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
    threadgroup_barrier(mem_flags::mem_threadgroup);
}

/* ------------------------------------------------------------------ */
/*  Shared per-block body — templated on the loaded integer samples.    */
/* ------------------------------------------------------------------ */

/* Compute the masked-error float partial for one 8x8 block. `s_ref` /
 * `s_dist` hold the raw integer samples (already loaded); `dct_s` /
 * `dct_d` / `z_s` / `z_d` are threadgroup scratch. Returns the block
 * partial via `partials_out[slot]`, written only by thread 0. */
static void psnr_hvs_block_body(threadgroup int *s_ref, threadgroup int *s_dist,
                                threadgroup int *dct_s, threadgroup int *dct_d,
                                threadgroup int *z_s, threadgroup int *z_d,
                                const constant float *csf, device float *partials_out,
                                uint slot, uint local_idx, bool valid_block)
{
    float s_means[4] = {0.f, 0.f, 0.f, 0.f};
    float d_means[4] = {0.f, 0.f, 0.f, 0.f};
    float s_vars[4] = {0.f, 0.f, 0.f, 0.f};
    float d_vars[4] = {0.f, 0.f, 0.f, 0.f};
    float s_gmean = 0.f, d_gmean = 0.f;
    float s_gvar = 0.f, d_gvar = 0.f;
    float s_mc = 0.f, d_mc = 0.f;

    if (local_idx == 0u) {
        /* Pass 1: means (CPU i,j summation order). */
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

    /* Integer DCT in place, parallel across the first eight lanes. */
    od_bin_fdct8x8_parallel(dct_s, z_s, local_idx);
    od_bin_fdct8x8_parallel(dct_d, z_d, local_idx);

    if (local_idx != 0u)
        return;

    /* Per-coefficient masking table mask[i][j] = (csf*0.3885…)^2. */
    float mask[64];
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            const float c = csf[i * 8 + j];
            const float m = c * 0.3885746225901003f;
            mask[i * 8 + j] = m * m;
        }
    }

    /* Pass 3: per-coefficient mask·dct^2 accumulation, skipping DC. */
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
    float sm = sqrt(s_mc * s_gvar) / 32.f;
    const float dm = sqrt(d_mc * d_gvar) / 32.f;
    if (dm > sm)
        sm = dm;
    const float thresh = sm;

    /* Pass 4: per-coefficient masked-error contribution. */
    float ret = 0.f;
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            const float c = csf[i * 8 + j];
            float err = fabs((float)dct_s[i * 8 + j] - (float)dct_d[i * 8 + j]);
            if (i != 0 || j != 0) {
                const float t = thresh / mask[i * 8 + j];
                err = err < t ? 0.f : err - t;
            }
            ret += (err * c) * (err * c);
        }
    }
    if (!valid_block)
        ret = 0.f;

    partials_out[slot] = ret;
}

/* ------------------------------------------------------------------ */
/*  8 bpc kernel                                                        */
/* ------------------------------------------------------------------ */
kernel void integer_psnr_hvs_8bpc(
    const device uchar  *ref      [[buffer(0)]],
    const device uchar  *dis      [[buffer(1)]],
    device       float  *partials [[buffer(2)]],
    const constant float *csf     [[buffer(3)]],
    constant     uint4  &dims     [[buffer(4)]],
    constant     uint2  &strides  [[buffer(5)]],
    uint2 bid       [[threadgroup_position_in_grid]],
    uint2 lpos      [[thread_position_in_threadgroup]],
    uint  local_idx [[thread_index_in_threadgroup]])
{
    threadgroup int s_ref[64];
    threadgroup int s_dist[64];
    threadgroup int dct_s[64];
    threadgroup int dct_d[64];
    threadgroup int z_s[64];
    threadgroup int z_d[64];

    const uint width  = dims.x;
    const uint height = dims.y;
    const uint num_blocks_x = dims.z;
    const uint num_blocks_y = dims.w;

    const uint blk_x = bid.x;
    const uint blk_y = bid.y;
    const uint lx = lpos.x;
    const uint ly = lpos.y;

    const uint x0 = blk_x * 7u;
    const uint y0 = blk_y * 7u;
    const bool valid_block =
        (blk_x < num_blocks_x && blk_y < num_blocks_y && x0 + 7u < width && y0 + 7u < height);

    int my_ref = 0;
    int my_dist = 0;
    if (valid_block) {
        const uint sx = x0 + lx;
        const uint sy = y0 + ly;
        my_ref  = (int)ref[sy * strides.x + sx];
        my_dist = (int)dis[sy * strides.y + sx];
    }
    s_ref[local_idx]  = my_ref;
    s_dist[local_idx] = my_dist;
    dct_s[local_idx]  = my_ref;
    dct_d[local_idx]  = my_dist;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    const uint slot = blk_y * num_blocks_x + blk_x;
    psnr_hvs_block_body(s_ref, s_dist, dct_s, dct_d, z_s, z_d, csf, partials, slot, local_idx,
                        valid_block);
}

/* ------------------------------------------------------------------ */
/*  16 bpc kernel                                                       */
/* ------------------------------------------------------------------ */
kernel void integer_psnr_hvs_16bpc(
    const device uchar  *ref      [[buffer(0)]],
    const device uchar  *dis      [[buffer(1)]],
    device       float  *partials [[buffer(2)]],
    const constant float *csf     [[buffer(3)]],
    constant     uint4  &dims     [[buffer(4)]],
    constant     uint2  &strides  [[buffer(5)]],
    uint2 bid       [[threadgroup_position_in_grid]],
    uint2 lpos      [[thread_position_in_threadgroup]],
    uint  local_idx [[thread_index_in_threadgroup]])
{
    threadgroup int s_ref[64];
    threadgroup int s_dist[64];
    threadgroup int dct_s[64];
    threadgroup int dct_d[64];
    threadgroup int z_s[64];
    threadgroup int z_d[64];

    const uint width  = dims.x;
    const uint height = dims.y;
    const uint num_blocks_x = dims.z;
    const uint num_blocks_y = dims.w;

    const uint blk_x = bid.x;
    const uint blk_y = bid.y;
    const uint lx = lpos.x;
    const uint ly = lpos.y;

    const uint x0 = blk_x * 7u;
    const uint y0 = blk_y * 7u;
    const bool valid_block =
        (blk_x < num_blocks_x && blk_y < num_blocks_y && x0 + 7u < width && y0 + 7u < height);

    int my_ref = 0;
    int my_dist = 0;
    if (valid_block) {
        const uint sx = x0 + lx;
        const uint sy = y0 + ly;
        /* Raw 10/12-bit integer samples (no scaler division) — matches
         * the CPU reference, which feeds full-range values into the DCT
         * and divides the final score by samplemax^2. */
        const device ushort *ref_row = (const device ushort *)(ref + sy * strides.x);
        const device ushort *dis_row = (const device ushort *)(dis + sy * strides.y);
        my_ref  = (int)ref_row[sx];
        my_dist = (int)dis_row[sx];
    }
    s_ref[local_idx]  = my_ref;
    s_dist[local_idx] = my_dist;
    dct_s[local_idx]  = my_ref;
    dct_d[local_idx]  = my_dist;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    const uint slot = blk_y * num_blocks_x + blk_x;
    psnr_hvs_block_body(s_ref, s_dist, dct_s, dct_d, z_s, z_d, csf, partials, slot, local_idx,
                        valid_block);
}
