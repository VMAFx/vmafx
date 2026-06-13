/**
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  HIP port of the integer VIF filter + statistics kernels.
 *
 *  Direct functional port of libvmaf/src/feature/cuda/integer_vif/filter1d.cu
 *  and vif_statistics.cuh, simplified to a scalar-per-thread layout that
 *  drops the CUDA shared-memory tiling but retains the wavefront-level
 *  reduction to match the CUDA twin's warp-reduce + single-atomicAdd pattern.
 *  Numerical output matches the CPU integer_vif.c reference within places=4
 *  (the ADR-0214 cross-backend parity gate).
 *
 *  Algorithmic correspondence to the CUDA twin (key change vs. the broken
 *  pre-ADR-0537 HIP kernel):
 *
 *    1. Filter half-widths now come from `vif_filter1d_width[scale]/2`,
 *       NOT from the kernel-name suffix (which encodes both fwidth_0 and
 *       fwidth_rd — two distinct widths the old code conflated).
 *         scale 0: fwidth=17, half=8
 *         scale 1: fwidth= 9, half=4
 *         scale 2: fwidth= 5, half=2
 *         scale 3: fwidth= 3, half=1
 *
 *    2. Filter coefficients are uploaded once at init() time to a device
 *       buffer (4*18 = 144 bytes total) and passed to every kernel launch
 *       as `const uint16_t *vif_filt_dev`.  The pre-ADR-0537 kernel was
 *       being handed a host pointer, which caused the GPU memory access
 *       fault that motivated this fix.
 *
 *    3. The scale-0/1/2 vertical+horizontal kernel pair additionally
 *       computes the downsampling residual (the `*_convol` channels in
 *       CUDA), writing the half-resolution ref/dis planes that the next
 *       scale consumes.  The pre-ADR-0537 kernel skipped this step
 *       entirely, leaving scales 1..3 reading uninitialised device memory.
 *
 *    4. The 8-bit horizontal kernel now invokes the same statistic
 *       calculation as the 16-bit horizontal kernels (the pre-ADR-0537
 *       kernel called it with a placeholder cross-term, producing
 *       wrong-by-construction scores even when it didn't crash).
 *
 *    5. (ADR-0552, superseded by ADR-0563) ADR-0552 introduced a
 *       wavefront XOR-reduce (split-int64 lo/hi halves shuffled
 *       independently) to reduce atomicAdd contention.  That approach
 *       silently drops the carry from lo-half overflow when accumulating
 *       negative int64_t values: if the sum of lo halves across warpSize
 *       threads overflows uint32_t, the carry bit into the hi half is
 *       discarded.  For natural-video content num_log accumulates
 *       negative int32_t differences per pixel (log numerator < log
 *       denominator), whose uint32_t lo = ~0 pattern triggers the carry
 *       drop in every wavefront, producing VMAF ~57 instead of ~76 on
 *       the Netflix src01 reference pair.
 *
 *       ADR-0563 reverts to per-thread atomicAdd for all 7 accumulator
 *       fields, mirroring the ADR-0539 decision for integer_adm_hip.
 *       The 64-bit atomicAdd on AMD GFX hardware uses a full-width CAS
 *       and never loses carry.  The additional atomic pressure (128
 *       threads × 7 fields per kernel launch) is negligible relative
 *       to the per-pixel filter work.
 *
 *    6. (ADR-0568) Boundary reads previously used clamp_i (replicate-edge)
 *       in all filter loops.  The CPU reference path uses PADDING_SQ_DATA
 *       (a symmetric reflect: index -1 reads pixel 1, index w reads pixel
 *       w-2), and the CUDA twin uses the same two-bounce mirror in its
 *       shared-memory load stage.  The clamp_i mismatch produced a
 *       max |HIP−CPU| delta of ≈ 0.0018 per VIF scale (places~2.75) on
 *       the Netflix src01 576×324 pair, violating ADR-0214's places=4 gate.
 *       All filter-loop boundary reads now use mirror2_i, which drops the
 *       gap to places~6 (max delta ≈ 1e-6) across all four scales and all
 *       48 src01 frames.  Verified on gfx1030 (RDNA2, wave32) hardware.
 *
 *  Block layout (kept simple — no shared-memory tiling):
 *    - Vertical pass: BLOCK_X=32, BLOCK_Y=8, scalar per thread.
 *    - Horizontal pass + accumulation: BLOCK_X=128, BLOCK_Y=1,
 *      scalar per thread, per-thread atomicAdd (ADR-0563).
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

#include "hip/integer_vif_hip.h"

/* Filter widths per scale — matches libvmaf/src/feature/integer_vif.h
 * `vif_filter1d_width[4] = {17, 9, 5, 3}`. */
#define VIF_FW_S0 17
#define VIF_FW_S1  9
#define VIF_FW_S2  5
#define VIF_FW_S3  3

/* --------------------------------------------------------------------------
 * Bit-manipulation helpers — identical to the CUDA twin's vif_statistics.cuh.
 * -------------------------------------------------------------------------- */

__device__ __forceinline__ static uint16_t get_best16_from32(uint32_t temp, int *x)
{
    int k = __clz(temp);
    k = 16 - k;
    temp = temp >> k;
    *x = -k;
    return (uint16_t)temp;
}

__device__ __forceinline__ static uint16_t get_best16_from64(uint64_t temp, int *x)
{
    int k = __clzll(temp);
    if (k > 48) {
        k -= 48;
        temp = temp << k;
        *x = k;
    } else if (k < 47) {
        k = 48 - k;
        temp = temp >> k;
        *x = -k;
    } else {
        *x = 0;
        if (temp >> 16) {
            temp = temp >> 1;
            *x = -1;
        }
    }
    return (uint16_t)temp;
}

__device__ __forceinline__ static int32_t log_generate(int i)
{
    return (int32_t)__float2int_rn(log2f((float)i) * 2048.0f);
}

/* --------------------------------------------------------------------------
 * Per-pixel VIF statistic accumulation — scalar variant matching the CPU
 * integer_vif.c lines 300–348 (and the CUDA twin's vif_statistic_calculation
 * function called on per-thread scalars).
 * -------------------------------------------------------------------------- */

__device__ __forceinline__ static void vif_statistic_one(
    uint32_t mu1, uint32_t mu2, uint32_t xx_filt, uint32_t yy_filt, uint32_t xy_filt,
    double vif_enhn_gain_limit, vif_accums_hip *acc)
{
    const int32_t sigma_nsq = 65536 << 1;

    uint32_t mu1_sq  = (uint32_t)((((uint64_t)mu1 * mu1) + 2147483648ULL) >> 32);
    uint32_t mu2_sq  = (uint32_t)((((uint64_t)mu2 * mu2) + 2147483648ULL) >> 32);
    uint32_t mu1_mu2 = (uint32_t)((((uint64_t)mu1 * mu2) + 2147483648ULL) >> 32);

    int32_t sigma1_sq = (int32_t)(xx_filt - mu1_sq);
    int32_t sigma2_sq = (int32_t)(yy_filt - mu2_sq);
    int32_t sigma12   = (int32_t)(xy_filt - mu1_mu2);

    /* Clamp both sigmas to 0 — matches the CUDA twin's max(sigma1_sq, 0) /
     * max(sigma2_sq, 0) (vif_statistics.cuh lines 96-97).  Without the
     * sigma1_sq clamp, a negative sigma1_sq could pass the sigma_nsq threshold
     * test below and produce an invalid log_den_stage1 value. */
    if (sigma1_sq < 0) sigma1_sq = 0;
    if (sigma2_sq < 0) sigma2_sq = 0;

    /* Compute g and sv_sq unconditionally, mirroring the CUDA twin's
     * lines 102-116.  g defaults to 0, sv_sq defaults to sigma2_sq; g is
     * assigned only when sigma12 > 0, sigma1_sq != 0, and sigma2_sq != 0
     * (matching the CUDA twin's guard), then sv_sq is updated and clamped,
     * and g is clamped to vif_enhn_gain_limit. */
    const double eps = 65536 * 1.0e-10;
    double g = 0.0;
    int32_t sv_sq = sigma2_sq;

    if (sigma12 > 0 && sigma1_sq != 0 && sigma2_sq != 0)
        g = sigma12 / ((double)sigma1_sq + eps);
    sv_sq = (int32_t)(sigma2_sq - g * sigma12);
    if (sv_sq < (int32_t)eps) sv_sq = (int32_t)eps;
    if (g > vif_enhn_gain_limit) g = vif_enhn_gain_limit;

    if (sigma1_sq >= sigma_nsq) {
        uint32_t log_den_stage1 = (uint32_t)(sigma_nsq + sigma1_sq);
        int x;
        uint16_t log_den1 = get_best16_from32(log_den_stage1, &x);

        acc->num_x += 1;
        acc->x     += x;
        int32_t den_val = log_generate(log_den1);

        /* Use sigma12 >= 0 (not > 0 && sigma2_sq > 0) — matches the CUDA twin's
         * line 136.  sv_sq >= 0 and sigma_nsq > 0, so numer1 > 0 always; the
         * get_best16_from64 call cannot receive 0. */
        if (sigma12 >= 0) {
            uint32_t numer1     = (uint32_t)(sv_sq + sigma_nsq);
            int64_t  numer1_tmp = (int64_t)(g * g * sigma1_sq) + (int64_t)numer1;

            int x1, x2;
            uint16_t numlog = get_best16_from64((uint64_t)numer1_tmp, &x1);
            uint16_t denlog = get_best16_from64((uint64_t)numer1, &x2);
            acc->x2      += (x2 - x1);
            int64_t num_val = (int64_t)log_generate(numlog) - (int64_t)log_generate(denlog);
            acc->num_log += num_val;
            acc->den_log += den_val;
        } else {
            acc->num_log += 0;
            acc->den_log += den_val;
        }
    } else {
        acc->num_non_log += sigma2_sq;
        acc->den_non_log += 1;
    }
}

/* --------------------------------------------------------------------------
 * Boundary helpers.
 * -------------------------------------------------------------------------- */

/* Two-bounce mirror reflect — matches the CPU reference's PADDING_SQ_DATA
 * (integer_vif.h lines 95-118) and the CUDA twin's boundary load in
 * filter1d.cu (lines 121-128, "Two-bounce mirror: reflect at top then at
 * bottom").
 *
 * The CPU pads the temporary buffer symmetrically before the horizontal
 * pass: index -1 reads pixel 1, index w reads pixel w-2.  Using clamp_i
 * (replicate-edge) instead of the symmetric reflect is the root cause of
 * the ADR-0214 places=4 delta — on the Netflix src01 576×324 pair this
 * manifests as max |HIP−CPU| ≈ 0.0018 per VIF scale (places~2.75) vs
 * the required 1e-4.  Switching to mirror2_i drops the gap to places~6
 * (max |HIP−CPU| ≈ 1e-6) across all four scales and all 48 frames.
 *
 * A single bounce suffices for all real VIF filter widths (max half=8 for
 * scale-0 on a 576-pixel-wide frame, well within [0..dim-1] after one
 * reflect).  The safety clamp handles degenerate frames where dim is
 * smaller than the filter half-width, mirroring the CUDA twin's fallback.
 *
 * ADR-0568: this is the fix that achieves per-feature places=4 for
 * integer_vif_hip, superseding the clamp_i boundary policy documented in
 * the pre-fix header comment above atomicadd_accums. */
__device__ __forceinline__ static int mirror2_i(int v, int dim)
{
    if (v < 0)
        v = -v;
    if (v >= dim)
        v = 2 * dim - v - 2;
    /* Safety clamp for very small frames (dim < filter half-width). */
    if (v < 0)
        v = 0;
    if (v >= dim)
        v = dim - 1;
    return v;
}

/* --------------------------------------------------------------------------
 * Per-thread atomicAdd helper for all 7 vif_accums_hip fields (ADR-0563).
 *
 * ADR-0552 introduced a wavefront XOR-reduce (split-int64 lo/hi halves)
 * before a single lane-0 atomicAdd.  That approach silently drops the carry
 * from lo-half overflow when accumulating negative int64_t values: if the
 * sum of lo halves across warpSize threads overflows uint32_t, the carry bit
 * into the hi half is discarded.  For natural-video content num_log
 * accumulates negative int32_t values per pixel (log numerator < log
 * denominator), whose uint32_t lo = ~0 pattern triggers the carry drop in
 * every wavefront.  Result: num_log/den_log are severely wrong, producing
 * integer_vif_scale{0..3} near zero instead of ~0.5, and VMAF ~57 instead
 * of ~76 on the Netflix src01 reference pair.
 *
 * Fix (mirrors ADR-0539 for integer_adm_hip): every thread issues its own
 * atomicAdd for all 7 fields.  The 64-bit atomicAdd on AMD GFX hardware is
 * internally a CAS loop, but the CAS is on the full 64-bit word, so no carry
 * is ever lost.  Per-thread atomicAdd trades some atomic-contention bandwidth
 * for correctness; the contention is acceptable because the block is 128
 * threads and each issues 7 atomics — 896 atomics vs. 4 (2 wavefronts × 7
 * fields) before.  For the horizontal-pass accumulator (one per frame scale,
 * reset between frames), the contention is insignificant compared to the
 * per-pixel filter work.
 *
 * The wavefront_reduce_i64 / wavefront_reduce_accums helpers are removed
 * entirely so no future caller can accidentally reintroduce the carry bug.
 * -------------------------------------------------------------------------- */

__device__ __forceinline__ static void atomicadd_accums(
    vif_accums_hip *dst, const vif_accums_hip *src)
{
    atomicAdd((unsigned long long *)&dst->x,           (unsigned long long)src->x);
    atomicAdd((unsigned long long *)&dst->x2,          (unsigned long long)src->x2);
    atomicAdd((unsigned long long *)&dst->num_x,       (unsigned long long)src->num_x);
    atomicAdd((unsigned long long *)&dst->num_log,     (unsigned long long)src->num_log);
    atomicAdd((unsigned long long *)&dst->den_log,     (unsigned long long)src->den_log);
    atomicAdd((unsigned long long *)&dst->num_non_log, (unsigned long long)src->num_non_log);
    atomicAdd((unsigned long long *)&dst->den_non_log, (unsigned long long)src->den_non_log);
}

extern "C" {

/* --------------------------------------------------------------------------
 * Vertical filter pass — 8-bit path (scale 0 only).
 * -------------------------------------------------------------------------- */

__global__ void filter1d_8_vertical_kernel_uint32_t_17_9(
    VifBufferHip buf,
    const uint8_t *__restrict__ ref_in,
    const uint8_t *__restrict__ dis_in,
    int w, int h,
    const uint16_t *__restrict__ vif_filt_dev)
{
    const int x = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = (int)(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= w || y >= h)
        return;

    const int FW = VIF_FW_S0;
    const int HALF = FW / 2;
    const int FW_RD = VIF_FW_S1;
    const int RD_START = (FW - FW_RD) / 2;

    const uint16_t *filt    = &vif_filt_dev[0 * 18];
    const uint16_t *filt_rd = &vif_filt_dev[1 * 18];

    uint32_t accum_mu1 = 0, accum_mu2 = 0;
    uint32_t accum_ref = 0, accum_dis = 0, accum_ref_dis = 0;
    uint32_t accum_ref_rd = 0, accum_dis_rd = 0;

    for (int fi = 0; fi < FW; ++fi) {
        int ky = mirror2_i(y - HALF + fi, h);
        uint32_t r = ref_in[(size_t)ky * (size_t)buf.stride + (size_t)x];
        uint32_t d = dis_in[(size_t)ky * (size_t)buf.stride + (size_t)x];
        uint32_t coeff = filt[fi];
        uint32_t cr = coeff * r;
        uint32_t cd = coeff * d;
        accum_mu1 += cr;
        accum_mu2 += cd;
        accum_ref += cr * r;
        accum_dis += cd * d;
        accum_ref_dis += cr * d;

        if (fi >= RD_START && fi < RD_START + FW_RD) {
            uint32_t cr_rd = (uint32_t)filt_rd[fi - RD_START];
            accum_ref_rd += cr_rd * r;
            accum_dis_rd += cr_rd * d;
        }
    }

    const int stmp = (int)(buf.stride_tmp / sizeof(uint32_t));
    buf.tmp.mu1[y * stmp + x]     = (accum_mu1 + 128u) >> 8;
    buf.tmp.mu2[y * stmp + x]     = (accum_mu2 + 128u) >> 8;
    buf.tmp.ref[y * stmp + x]     = accum_ref;
    buf.tmp.dis[y * stmp + x]     = accum_dis;
    buf.tmp.ref_dis[y * stmp + x] = accum_ref_dis;
    buf.tmp.ref_convol[y * stmp + x] = (accum_ref_rd + 128u) >> 8;
    buf.tmp.dis_convol[y * stmp + x] = (accum_dis_rd + 128u) >> 8;
}

__global__ void filter1d_8_horizontal_kernel_2_17_9(
    VifBufferHip buf,
    int w, int h,
    const uint16_t *__restrict__ vif_filt_dev,
    double vif_enhn_gain_limit,
    vif_accums_hip *accum_out)
{
    const int x = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = (int)(blockIdx.y * blockDim.y + threadIdx.y);

    const int FW = VIF_FW_S0;
    const int HALF = FW / 2;
    const int FW_RD = VIF_FW_S1;
    const int HALF_RD = FW_RD / 2;

    const uint16_t *filt    = &vif_filt_dev[0 * 18];
    const uint16_t *filt_rd = &vif_filt_dev[1 * 18];

    if (x < w && y < h) {
        const int stmp = (int)(buf.stride_tmp / sizeof(uint32_t));
        const int row  = y * stmp;

        uint32_t accum_mu1 = 0, accum_mu2 = 0;
        uint64_t accum_ref = 0, accum_dis = 0, accum_ref_dis = 0;

        for (int fj = 0; fj < FW; ++fj) {
            int kx = mirror2_i(x - HALF + fj, w);
            uint32_t coeff = (uint32_t)filt[fj];
            accum_mu1 += coeff * buf.tmp.mu1[row + kx];
            accum_mu2 += coeff * buf.tmp.mu2[row + kx];
            accum_ref += (uint64_t)coeff * (uint64_t)buf.tmp.ref[row + kx];
            accum_dis += (uint64_t)coeff * (uint64_t)buf.tmp.dis[row + kx];
            accum_ref_dis += (uint64_t)coeff * (uint64_t)buf.tmp.ref_dis[row + kx];
        }

        uint32_t mu1_v = accum_mu1;
        uint32_t mu2_v = accum_mu2;
        uint32_t xx_v  = (uint32_t)((accum_ref     + 32768) >> 16);
        uint32_t yy_v  = (uint32_t)((accum_dis     + 32768) >> 16);
        uint32_t xy_v  = (uint32_t)((accum_ref_dis + 32768) >> 16);

        /* ADR-0563: per-thread atomicAdd — replaces the broken wavefront
         * XOR-reduce that silently dropped carry bits when accumulating
         * negative int64_t num_log values (see comment above). */
        vif_accums_hip thr = {0, 0, 0, 0, 0, 0, 0};
        vif_statistic_one(mu1_v, mu2_v, xx_v, yy_v, xy_v, vif_enhn_gain_limit, &thr);
        atomicadd_accums(accum_out, &thr);
    }

    if (x < w && y < h && (y & 1) == 0 && (x & 1) == 0) {
        const int stmp = (int)(buf.stride_tmp / sizeof(uint32_t));
        const int row  = y * stmp;
        uint32_t accum_ref_rd = 0, accum_dis_rd = 0;
        for (int fj = 0; fj < FW_RD; ++fj) {
            int kx = mirror2_i(x - HALF_RD + fj, w);
            uint32_t coeff = (uint32_t)filt_rd[fj];
            accum_ref_rd += coeff * buf.tmp.ref_convol[row + kx];
            accum_dis_rd += coeff * buf.tmp.dis_convol[row + kx];
        }
        uint16_t mu_ref = (uint16_t)((accum_ref_rd + 32768u) >> 16);
        uint16_t mu_dis = (uint16_t)((accum_dis_rd + 32768u) >> 16);

        uint16_t *rd_ref = (uint16_t *)buf.ref;
        uint16_t *rd_dis = (uint16_t *)buf.dis;
        const ptrdiff_t rd_stride16 = buf.rd_stride / (ptrdiff_t)sizeof(uint16_t);
        rd_ref[(y / 2) * rd_stride16 + (x / 2)] = mu_ref;
        rd_dis[(y / 2) * rd_stride16 + (x / 2)] = mu_dis;
    }
}


} /* extern "C" — templates need C++ linkage */

template <int FW, int FW_RD>
__device__ __forceinline__ static void vif_vert_16_body(
    VifBufferHip &buf, const uint16_t *ref_in, const uint16_t *dis_in,
    int w, int h, int scale,
    int32_t add_shift_VP, int32_t shift_VP,
    int32_t add_shift_VP_sq, int32_t shift_VP_sq,
    const uint16_t *filt, const uint16_t *filt_rd)
{
    const int x = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = (int)(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= w || y >= h)
        return;

    const int HALF    = FW / 2;
    const int RD_START = (FW - FW_RD) / 2;

    const ptrdiff_t in_stride16 = (scale == 0)
        ? (buf.stride    / (ptrdiff_t)sizeof(uint16_t))
        : (buf.rd_stride / (ptrdiff_t)sizeof(uint16_t));

    uint32_t accum_mu1 = 0, accum_mu2 = 0;
    uint64_t accum_ref = 0, accum_dis = 0, accum_ref_dis = 0;
    uint32_t accum_ref_rd = 0, accum_dis_rd = 0;

    for (int fi = 0; fi < FW; ++fi) {
        int ky = mirror2_i(y - HALF + fi, h);
        uint32_t r = ref_in[(ptrdiff_t)ky * in_stride16 + x];
        uint32_t d = dis_in[(ptrdiff_t)ky * in_stride16 + x];
        uint32_t coeff = (uint32_t)filt[fi];
        uint32_t cr = coeff * r;
        uint32_t cd = coeff * d;
        accum_mu1 += cr;
        accum_mu2 += cd;
        accum_ref += (uint64_t)cr * r;
        accum_dis += (uint64_t)cd * d;
        accum_ref_dis += (uint64_t)cr * d;

        if (FW_RD > 0 && fi >= RD_START && fi < RD_START + FW_RD) {
            uint32_t cr_rd = (uint32_t)filt_rd[fi - RD_START];
            accum_ref_rd += cr_rd * r;
            accum_dis_rd += cr_rd * d;
        }
    }

    const int stmp = (int)(buf.stride_tmp / sizeof(uint32_t));
    buf.tmp.mu1[y * stmp + x]     = (accum_mu1 + (uint32_t)add_shift_VP) >> shift_VP;
    buf.tmp.mu2[y * stmp + x]     = (accum_mu2 + (uint32_t)add_shift_VP) >> shift_VP;
    buf.tmp.ref[y * stmp + x]     = (uint32_t)((accum_ref     + (uint64_t)add_shift_VP_sq) >> shift_VP_sq);
    buf.tmp.dis[y * stmp + x]     = (uint32_t)((accum_dis     + (uint64_t)add_shift_VP_sq) >> shift_VP_sq);
    buf.tmp.ref_dis[y * stmp + x] = (uint32_t)((accum_ref_dis + (uint64_t)add_shift_VP_sq) >> shift_VP_sq);
    if (FW_RD > 0) {
        buf.tmp.ref_convol[y * stmp + x] = (accum_ref_rd + (uint32_t)add_shift_VP) >> shift_VP;
        buf.tmp.dis_convol[y * stmp + x] = (accum_dis_rd + (uint32_t)add_shift_VP) >> shift_VP;
    }
}

template <int FW, int FW_RD>
__device__ __forceinline__ static void vif_hori_16_body(
    VifBufferHip &buf, int w, int h, int scale,
    int32_t add_shift_HP, int32_t shift_HP,
    const uint16_t *filt, const uint16_t *filt_rd,
    double vif_enhn_gain_limit,
    vif_accums_hip *accum_out)
{
    (void)scale;
    const int x = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = (int)(blockIdx.y * blockDim.y + threadIdx.y);

    const int HALF    = FW / 2;
    const int HALF_RD = FW_RD / 2;

    if (x < w && y < h) {
        const int stmp = (int)(buf.stride_tmp / sizeof(uint32_t));
        const int row  = y * stmp;

        uint32_t accum_mu1 = 0, accum_mu2 = 0;
        uint64_t accum_ref = 0, accum_dis = 0, accum_ref_dis = 0;

        for (int fj = 0; fj < FW; ++fj) {
            int kx = mirror2_i(x - HALF + fj, w);
            uint32_t coeff = (uint32_t)filt[fj];
            accum_mu1 += coeff * buf.tmp.mu1[row + kx];
            accum_mu2 += coeff * buf.tmp.mu2[row + kx];
            accum_ref += (uint64_t)coeff * (uint64_t)buf.tmp.ref[row + kx];
            accum_dis += (uint64_t)coeff * (uint64_t)buf.tmp.dis[row + kx];
            accum_ref_dis += (uint64_t)coeff * (uint64_t)buf.tmp.ref_dis[row + kx];
        }

        uint32_t mu1_v = accum_mu1;
        uint32_t mu2_v = accum_mu2;
        uint32_t xx_v  = (uint32_t)((accum_ref     + (uint64_t)add_shift_HP) >> shift_HP);
        uint32_t yy_v  = (uint32_t)((accum_dis     + (uint64_t)add_shift_HP) >> shift_HP);
        uint32_t xy_v  = (uint32_t)((accum_ref_dis + (uint64_t)add_shift_HP) >> shift_HP);

        /* ADR-0563: per-thread atomicAdd — replaces the broken wavefront
         * XOR-reduce that silently dropped carry bits when accumulating
         * negative int64_t num_log values (see comment above). */
        vif_accums_hip thr = {0, 0, 0, 0, 0, 0, 0};
        vif_statistic_one(mu1_v, mu2_v, xx_v, yy_v, xy_v, vif_enhn_gain_limit, &thr);
        atomicadd_accums(accum_out, &thr);
    }

    if (FW_RD > 0 && x < w && y < h && (y & 1) == 0 && (x & 1) == 0) {
        const int stmp_rd = (int)(buf.stride_tmp / sizeof(uint32_t));
        const int row_rd  = y * stmp_rd;
        uint32_t accum_ref_rd = 0, accum_dis_rd = 0;
        for (int fj = 0; fj < FW_RD; ++fj) {
            int kx = mirror2_i(x - HALF_RD + fj, w);
            uint32_t coeff = (uint32_t)filt_rd[fj];
            accum_ref_rd += coeff * buf.tmp.ref_convol[row_rd + kx];
            accum_dis_rd += coeff * buf.tmp.dis_convol[row_rd + kx];
        }
        uint16_t mu_ref = (uint16_t)((accum_ref_rd + 32768u) >> 16);
        uint16_t mu_dis = (uint16_t)((accum_dis_rd + 32768u) >> 16);

        uint16_t *rd_ref = (uint16_t *)buf.ref;
        uint16_t *rd_dis = (uint16_t *)buf.dis;
        const ptrdiff_t rd_stride16 = buf.rd_stride / (ptrdiff_t)sizeof(uint16_t);
        rd_ref[(y / 2) * rd_stride16 + (x / 2)] = mu_ref;
        rd_dis[(y / 2) * rd_stride16 + (x / 2)] = mu_dis;
    }
}


extern "C" {

__global__ void filter1d_16_vertical_kernel_uint2_17_9_0(
    VifBufferHip buf, const uint16_t *__restrict__ ref_in, const uint16_t *__restrict__ dis_in,
    int w, int h,
    int32_t add_shift_VP, int32_t shift_VP, int32_t add_shift_VP_sq, int32_t shift_VP_sq,
    const uint16_t *__restrict__ vif_filt_dev)
{
    vif_vert_16_body<VIF_FW_S0, VIF_FW_S1>(buf, ref_in, dis_in, w, h, 0,
        add_shift_VP, shift_VP, add_shift_VP_sq, shift_VP_sq,
        &vif_filt_dev[0 * 18], &vif_filt_dev[1 * 18]);
}

__global__ void filter1d_16_vertical_kernel_uint2_9_5_1(
    VifBufferHip buf, const uint16_t *__restrict__ ref_in, const uint16_t *__restrict__ dis_in,
    int w, int h,
    int32_t add_shift_VP, int32_t shift_VP, int32_t add_shift_VP_sq, int32_t shift_VP_sq,
    const uint16_t *__restrict__ vif_filt_dev)
{
    vif_vert_16_body<VIF_FW_S1, VIF_FW_S2>(buf, ref_in, dis_in, w, h, 1,
        add_shift_VP, shift_VP, add_shift_VP_sq, shift_VP_sq,
        &vif_filt_dev[1 * 18], &vif_filt_dev[2 * 18]);
}

__global__ void filter1d_16_vertical_kernel_uint2_5_3_2(
    VifBufferHip buf, const uint16_t *__restrict__ ref_in, const uint16_t *__restrict__ dis_in,
    int w, int h,
    int32_t add_shift_VP, int32_t shift_VP, int32_t add_shift_VP_sq, int32_t shift_VP_sq,
    const uint16_t *__restrict__ vif_filt_dev)
{
    vif_vert_16_body<VIF_FW_S2, VIF_FW_S3>(buf, ref_in, dis_in, w, h, 2,
        add_shift_VP, shift_VP, add_shift_VP_sq, shift_VP_sq,
        &vif_filt_dev[2 * 18], &vif_filt_dev[3 * 18]);
}

__global__ void filter1d_16_vertical_kernel_uint2_3_0_3(
    VifBufferHip buf, const uint16_t *__restrict__ ref_in, const uint16_t *__restrict__ dis_in,
    int w, int h,
    int32_t add_shift_VP, int32_t shift_VP, int32_t add_shift_VP_sq, int32_t shift_VP_sq,
    const uint16_t *__restrict__ vif_filt_dev)
{
    vif_vert_16_body<VIF_FW_S3, 0>(buf, ref_in, dis_in, w, h, 3,
        add_shift_VP, shift_VP, add_shift_VP_sq, shift_VP_sq,
        &vif_filt_dev[3 * 18], nullptr);
}

__global__ void filter1d_16_horizontal_kernel_2_17_9_0(
    VifBufferHip buf, int w, int h,
    int32_t add_shift_HP, int32_t shift_HP,
    const uint16_t *__restrict__ vif_filt_dev,
    double vif_enhn_gain_limit,
    vif_accums_hip *accum_out)
{
    vif_hori_16_body<VIF_FW_S0, VIF_FW_S1>(buf, w, h, 0,
        add_shift_HP, shift_HP,
        &vif_filt_dev[0 * 18], &vif_filt_dev[1 * 18],
        vif_enhn_gain_limit, accum_out);
}

__global__ void filter1d_16_horizontal_kernel_2_9_5_1(
    VifBufferHip buf, int w, int h,
    int32_t add_shift_HP, int32_t shift_HP,
    const uint16_t *__restrict__ vif_filt_dev,
    double vif_enhn_gain_limit,
    vif_accums_hip *accum_out)
{
    vif_hori_16_body<VIF_FW_S1, VIF_FW_S2>(buf, w, h, 1,
        add_shift_HP, shift_HP,
        &vif_filt_dev[1 * 18], &vif_filt_dev[2 * 18],
        vif_enhn_gain_limit, accum_out);
}

__global__ void filter1d_16_horizontal_kernel_2_5_3_2(
    VifBufferHip buf, int w, int h,
    int32_t add_shift_HP, int32_t shift_HP,
    const uint16_t *__restrict__ vif_filt_dev,
    double vif_enhn_gain_limit,
    vif_accums_hip *accum_out)
{
    vif_hori_16_body<VIF_FW_S2, VIF_FW_S3>(buf, w, h, 2,
        add_shift_HP, shift_HP,
        &vif_filt_dev[2 * 18], &vif_filt_dev[3 * 18],
        vif_enhn_gain_limit, accum_out);
}

__global__ void filter1d_16_horizontal_kernel_2_3_0_3(
    VifBufferHip buf, int w, int h,
    int32_t add_shift_HP, int32_t shift_HP,
    const uint16_t *__restrict__ vif_filt_dev,
    double vif_enhn_gain_limit,
    vif_accums_hip *accum_out)
{
    vif_hori_16_body<VIF_FW_S3, 0>(buf, w, h, 3,
        add_shift_HP, shift_HP,
        &vif_filt_dev[3 * 18], nullptr,
        vif_enhn_gain_limit, accum_out);
}

} /* extern "C" */
