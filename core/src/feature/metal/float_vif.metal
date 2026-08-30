/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Metal compute kernels for the float_vif feature extractor.
 *  MSL port of the CUDA twin
 *  `core/src/feature/cuda/float_vif/float_vif_score.cu` and the SYCL twin
 *  `core/src/feature/sycl/float_vif_sycl.cpp` — same 4-scale separable
 *  Gaussian pyramid, same per-pixel `vif_statistic` (matching_matlab mode),
 *  same per-workgroup (num, den) float partial-sum reduction. The host
 *  (`float_vif_metal.mm`) sums the per-WG partials in double precision and
 *  divides num/den per scale, exactly like the CUDA/SYCL collect paths.
 *
 *  Two kernel functions, dispatched in order by `float_vif_metal.mm`:
 *
 *   1. `float_vif_compute` — reads ref/dis at this scale, applies the
 *      separable V then H Gaussian inline (5 accumulators: mu1, mu2,
 *      ref^2, dis^2, ref*dis), runs the per-pixel VIF statistic, and
 *      reduces (num, den) per threadgroup into one float each.
 *      Scale 0 reads the raw uint8/uint16 plane and converts inline
 *      (`is_raw`); scales 1..3 read the decimated float buffers.
 *
 *   2. `float_vif_decimate` — applies this scale's filter at the PREVIOUS
 *      scale's full dimensions, sampling at (2*gx, 2*gy) for the output
 *      (CPU's VIF_OPT_HANDLE_BORDERS branch). Scale 1 reads raw; scales
 *      2..3 read the previous float buffer.
 *
 *  bpc handling (8 vs 16): a single compute / decimate kernel reads the raw
 *  plane through `fvif_read_raw`, which branches on `bpc` (8 -> uint8 - 128;
 *  10/12/16 -> uint16 / scaler - 128). This is the SAME single-kernel,
 *  runtime-bpc strategy the CUDA and SYCL float_vif twins use (float_vif's
 *  accumulators are float at every bpc, so there is no int-vs-long
 *  accumulator split that the integer Metal twins need separate functions
 *  for). Keeping one kernel keeps the math byte-for-byte identical to the
 *  proven-parity CUDA/SYCL float twins.
 *
 *  Reduction: per-thread (num, den) -> simd_sum into an 8-slot threadgroup
 *  array -> one float partial per threadgroup per output, written to
 *  `num_partials[bid.y * grid_x + bid.x]` / `den_partials[...]`. No 64-bit
 *  atomics (MSL lacks atomic_fetch_add on long/double on Apple Silicon),
 *  matching the float_psnr / float_ms_ssim Metal reduction pattern.
 *
 *  Numeric contract: hardcoded default params (kernelscale=1.0,
 *  vif_sigma_nsq=2.0, vif_enhn_gain_limit=100.0, eps=1e-10), identical to
 *  the CUDA/SYCL twins; the .mm rejects any non-default option at init.
 *  Parity target places=4 (1e-4, ADR-0214) — same gate the CUDA/SYCL
 *  float_vif parity tests assert.
 */

#include <metal_stdlib>
using namespace metal;

/* Filter footprint per scale: {17, 9, 5, 3}. Constant-folded for the
 * default kernelscale=1.0, byte-identical to the CUDA/SYCL twins. */
#define FVIF_BX 16
#define FVIF_BY 16
#define FVIF_MAX_FW 17
#define FVIF_MAX_HFW 8
/* Worst-case tile pitch (WG + 2*max_hfw) = 16 + 16 = 32. */
#define FVIF_MAX_TILE_W (FVIF_BX + 2 * FVIF_MAX_HFW)

/* Per-scale Gaussian coefficients (kernelscale=1.0). Identical arrays to
 * FVIF_COEFF_S0..S3 in the CUDA/SYCL twins. */
constant float FVIF_COEFF_S0[FVIF_MAX_FW] = {
    0.00745626912f, 0.0142655009f, 0.0250313189f, 0.0402820669f, 0.0594526194f, 0.0804751068f,
    0.0999041125f,  0.113746084f,  0.118773937f,  0.113746084f,  0.0999041125f, 0.0804751068f,
    0.0594526194f,  0.0402820669f, 0.0250313189f, 0.0142655009f, 0.00745626912f};
constant float FVIF_COEFF_S1[FVIF_MAX_FW] = {
    0.0189780835f, 0.0558981746f, 0.120920904f,  0.192116052f, 0.224173605f, 0.192116052f,
    0.120920904f,  0.0558981746f, 0.0189780835f, 0.0f,         0.0f,         0.0f,
    0.0f,          0.0f,          0.0f,          0.0f,         0.0f};
constant float FVIF_COEFF_S2[FVIF_MAX_FW] = {
    0.054488685f, 0.244201347f, 0.402619958f, 0.244201347f, 0.054488685f, 0.0f, 0.0f, 0.0f, 0.0f,
    0.0f,         0.0f,         0.0f,         0.0f,         0.0f,         0.0f, 0.0f, 0.0f};
constant float FVIF_COEFF_S3[FVIF_MAX_FW] = {
    0.166378498f, 0.667243004f, 0.166378498f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    0.0f,         0.0f,         0.0f,         0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

inline constant float *fvif_coeffs(int scale)
{
    if (scale == 0) {
        return FVIF_COEFF_S0;
    }
    if (scale == 1) {
        return FVIF_COEFF_S1;
    }
    if (scale == 2) {
        return FVIF_COEFF_S2;
    }
    return FVIF_COEFF_S3;
}

inline int fvif_fw(int scale)
{
    const int fw[4] = {17, 9, 5, 3};
    return fw[scale];
}

/* Mirror padding. Both axes use the AVX2 border-path mirror
 * (2*sup - idx - 2) so the GPU matches the CPU production path — same as
 * the CUDA/SYCL twins (fvif_mirror_v / fvif_mirror_h are identical there). */
inline int fvif_mirror(int idx, int sup)
{
    if (idx < 0) {
        return -idx;
    }
    if (idx >= sup) {
        return 2 * sup - idx - 2;
    }
    return idx;
}

/* Read a raw uint8/uint16 pixel and convert to float in [-128, peak-128].
 * stride_bytes is the row stride in bytes. Matches CUDA fvif_read_raw and
 * SYCL read_raw exactly. */
inline float fvif_read_raw(const device uchar *plane, uint stride_bytes, int y, int x, uint bpc)
{
    if (bpc <= 8u) {
        return (float)plane[(uint)y * stride_bytes + (uint)x] - 128.0f;
    }
    const device ushort *row = (const device ushort *)(plane + (uint)y * stride_bytes);
    float scaler = 1.0f;
    if (bpc == 10u) {
        scaler = 4.0f;
    } else if (bpc == 12u) {
        scaler = 16.0f;
    } else if (bpc == 16u) {
        scaler = 256.0f;
    }
    return (float)row[(uint)x] / scaler - 128.0f;
}

/* ------------------------------------------------------------------ */
/*  Kernel 1: float_vif_compute                                        */
/*                                                                      */
/*  Buffer bindings:                                                    */
/*   [[buffer(0)]] ref_raw  — const uchar * raw ref plane (scale 0)    */
/*   [[buffer(1)]] dis_raw  — const uchar * raw dis plane (scale 0)    */
/*   [[buffer(2)]] ref_f    — const float * ref at this scale (s>=1)  */
/*   [[buffer(3)]] dis_f    — const float * dis at this scale (s>=1)  */
/*   [[buffer(4)]] num_part — float * per-WG num partial               */
/*   [[buffer(5)]] den_part — float * per-WG den partial               */
/*   [[buffer(6)]] params   — uint4 (.x=width, .y=height,             */
/*                                    .z=f_stride_floats, .w=bpc)      */
/*   [[buffer(7)]] cfg      — uint4 (.x=scale, .y=raw_stride_bytes,    */
/*                                    .z=grid_x, .w=0)                 */
/*  Grid: ceil(width/16) x ceil(height/16),  threads 16x16.           */
/* ------------------------------------------------------------------ */
kernel void float_vif_compute(
    const device uchar *ref_raw [[buffer(0)]], const device uchar *dis_raw [[buffer(1)]],
    const device float *ref_f [[buffer(2)]], const device float *dis_f [[buffer(3)]],
    device float *num_part [[buffer(4)]], device float *den_part [[buffer(5)]],
    constant uint4 &params [[buffer(6)]], constant uint4 &cfg [[buffer(7)]],
    uint2 gid [[thread_position_in_grid]], uint2 bid [[threadgroup_position_in_grid]],
    uint2 lpos [[thread_position_in_threadgroup]], uint lid [[thread_index_in_threadgroup]],
    uint simd_lane [[thread_index_in_simdgroup]], uint simd_id [[simdgroup_index_in_threadgroup]],
    uint simd_count [[simdgroups_per_threadgroup]])
{
    const int width = (int)params.x;
    const int height = (int)params.y;
    const uint f_stride = params.z;
    const uint bpc = params.w;
    const int scale = (int)cfg.x;
    const uint raw_stride = cfg.y;
    const uint grid_x = cfg.z;

    const int fw = fvif_fw(scale);
    const int hfw = fw / 2;
    constant float *coeff = fvif_coeffs(scale);

    const int gx = (int)gid.x;
    const int gy = (int)gid.y;
    const int lx = (int)lpos.x;
    const int ly = (int)lpos.y;
    const bool valid = (gx < width && gy < height);
    const bool is_raw = (scale == 0);

    const int tile_w = FVIF_BX + 2 * hfw;
    const int tile_h = FVIF_BY + 2 * hfw;
    const int tile_oy = (int)bid.y * FVIF_BY - hfw;
    const int tile_ox = (int)bid.x * FVIF_BX - hfw;
    const int tile_elems = tile_h * tile_w;

    /* Worst-case static tile allocation; only (16 + 2*hfw)^2 cells used. */
    threadgroup float s_ref[FVIF_MAX_TILE_W * FVIF_MAX_TILE_W];
    threadgroup float s_dis[FVIF_MAX_TILE_W * FVIF_MAX_TILE_W];
    threadgroup float s_v_mu1[FVIF_BY * FVIF_MAX_TILE_W];
    threadgroup float s_v_mu2[FVIF_BY * FVIF_MAX_TILE_W];
    threadgroup float s_v_xx[FVIF_BY * FVIF_MAX_TILE_W];
    threadgroup float s_v_yy[FVIF_BY * FVIF_MAX_TILE_W];
    threadgroup float s_v_xy[FVIF_BY * FVIF_MAX_TILE_W];

    /* Phase 1: cooperative tile load with mirror padding. */
    for (int i = (int)lid; i < tile_elems; i += FVIF_BX * FVIF_BY) {
        const int tr = i / tile_w;
        const int tc = i - tr * tile_w;
        const int py = fvif_mirror(tile_oy + tr, height);
        const int px = fvif_mirror(tile_ox + tc, width);
        float r, d;
        if (is_raw) {
            r = fvif_read_raw(ref_raw, raw_stride, py, px, bpc);
            d = fvif_read_raw(dis_raw, raw_stride, py, px, bpc);
        } else {
            r = ref_f[(uint)py * f_stride + (uint)px];
            d = dis_f[(uint)py * f_stride + (uint)px];
        }
        s_ref[tr * FVIF_MAX_TILE_W + tc] = r;
        s_dis[tr * FVIF_MAX_TILE_W + tc] = d;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    /* Phase 2: vertical filter for WG_Y output rows x tile_w cols. */
    const int vert_total = FVIF_BY * tile_w;
    for (int i = (int)lid; i < vert_total; i += FVIF_BX * FVIF_BY) {
        const int r = i / tile_w;
        const int c = i - r * tile_w;
        float a_mu1 = 0.0f, a_mu2 = 0.0f, a_xx = 0.0f, a_yy = 0.0f, a_xy = 0.0f;
        for (int k = 0; k < fw; ++k) {
            const float c_k = coeff[k];
            const float ref_v = s_ref[(r + k) * FVIF_MAX_TILE_W + c];
            const float dis_v = s_dis[(r + k) * FVIF_MAX_TILE_W + c];
            a_mu1 += c_k * ref_v;
            a_mu2 += c_k * dis_v;
            a_xx += c_k * (ref_v * ref_v);
            a_yy += c_k * (dis_v * dis_v);
            a_xy += c_k * (ref_v * dis_v);
        }
        s_v_mu1[r * FVIF_MAX_TILE_W + c] = a_mu1;
        s_v_mu2[r * FVIF_MAX_TILE_W + c] = a_mu2;
        s_v_xx[r * FVIF_MAX_TILE_W + c] = a_xx;
        s_v_yy[r * FVIF_MAX_TILE_W + c] = a_yy;
        s_v_xy[r * FVIF_MAX_TILE_W + c] = a_xy;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    /* Phase 3: per-thread horizontal filter + VIF statistic. */
    float my_num = 0.0f, my_den = 0.0f;
    if (valid) {
        float mu1 = 0.0f, mu2 = 0.0f, xx = 0.0f, yy = 0.0f, xy = 0.0f;
        for (int k = 0; k < fw; ++k) {
            const float c_k = coeff[k];
            mu1 += c_k * s_v_mu1[ly * FVIF_MAX_TILE_W + (lx + k)];
            mu2 += c_k * s_v_mu2[ly * FVIF_MAX_TILE_W + (lx + k)];
            xx += c_k * s_v_xx[ly * FVIF_MAX_TILE_W + (lx + k)];
            yy += c_k * s_v_yy[ly * FVIF_MAX_TILE_W + (lx + k)];
            xy += c_k * s_v_xy[ly * FVIF_MAX_TILE_W + (lx + k)];
        }

        const float eps = 1.0e-10f;
        const float vif_sigma_nsq = 2.0f;
        const float vif_egl = 100.0f;
        const float sigma_max_inv = (2.0f * 2.0f) / (255.0f * 255.0f);

        float sigma1_sq = xx - mu1 * mu1;
        float sigma2_sq = yy - mu2 * mu2;
        float sigma12 = xy - mu1 * mu2;
        sigma1_sq = max(sigma1_sq, 0.0f);
        sigma2_sq = max(sigma2_sq, 0.0f);
        float g = sigma12 / (sigma1_sq + eps);
        float sv_sq = sigma2_sq - g * sigma12;
        if (sigma1_sq < eps) {
            g = 0.0f;
            sv_sq = sigma2_sq;
            sigma1_sq = 0.0f;
        }
        if (sigma2_sq < eps) {
            g = 0.0f;
            sv_sq = 0.0f;
        }
        if (g < 0.0f) {
            sv_sq = sigma2_sq;
            g = 0.0f;
        }
        sv_sq = max(sv_sq, eps);
        g = min(g, vif_egl);

        float num_val = log2(1.0f + (g * g * sigma1_sq) / (sv_sq + vif_sigma_nsq));
        float den_val = log2(1.0f + sigma1_sq / vif_sigma_nsq);
        if (sigma12 < 0.0f) {
            num_val = 0.0f;
        }
        if (sigma1_sq < vif_sigma_nsq) {
            num_val = 1.0f - sigma2_sq * sigma_max_inv;
            den_val = 1.0f;
        }
        my_num = num_val;
        my_den = den_val;
    }

    /* Phase 4: SIMD-group -> threadgroup partials -> one float per WG.
     * No 64-bit atomics (MSL lacks them on Apple Silicon); same float
     * partial-sum reduction the float_psnr / float_ms_ssim kernels use. */
    threadgroup float sg_num[FVIF_BX * FVIF_BY / 32];
    threadgroup float sg_den[FVIF_BX * FVIF_BY / 32];

    const float lane_num = simd_sum(my_num);
    const float lane_den = simd_sum(my_den);
    if (simd_lane == 0) {
        sg_num[simd_id] = lane_num;
        sg_den[simd_id] = lane_den;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (lid == 0) {
        float total_num = 0.0f, total_den = 0.0f;
        for (uint i = 0; i < simd_count; ++i) {
            total_num += sg_num[i];
            total_den += sg_den[i];
        }
        const uint wg_idx = bid.y * grid_x + bid.x;
        num_part[wg_idx] = total_num;
        den_part[wg_idx] = total_den;
    }
}

/* ------------------------------------------------------------------ */
/*  Kernel 2: float_vif_decimate                                       */
/*                                                                      */
/*  Applies SCALE's filter at the PREVIOUS scale's dimensions, sampling */
/*  at (2*gx, 2*gy) for output. V-inner / H-outer ordering matches the  */
/*  CPU vif_filter1d_s and the CUDA/SYCL twins.                         */
/*                                                                      */
/*  Buffer bindings:                                                    */
/*   [[buffer(0)]] ref_raw  — const uchar * raw ref plane (scale 1)    */
/*   [[buffer(1)]] dis_raw  — const uchar * raw dis plane (scale 1)    */
/*   [[buffer(2)]] ref_f    — const float * prev-scale ref (s>=2)     */
/*   [[buffer(3)]] dis_f    — const float * prev-scale dis (s>=2)     */
/*   [[buffer(4)]] ref_out  — float * decimated ref out                */
/*   [[buffer(5)]] dis_out  — float * decimated dis out                */
/*   [[buffer(6)]] dims     — uint4 (.x=out_w, .y=out_h,              */
/*                                    .z=in_w, .w=in_h)                */
/*   [[buffer(7)]] cfg      — uint4 (.x=scale, .y=raw_stride_bytes,    */
/*                                    .z=f_stride_floats,              */
/*                                    .w=out_stride_floats)            */
/*   [[buffer(8)]] bpc4     — uint4 (.x=bpc, rest 0)                  */
/*  Grid: ceil(out_w/16) x ceil(out_h/16),  threads 16x16.            */
/* ------------------------------------------------------------------ */
kernel void
float_vif_decimate(const device uchar *ref_raw [[buffer(0)]],
                   const device uchar *dis_raw [[buffer(1)]],
                   const device float *ref_f [[buffer(2)]], const device float *dis_f [[buffer(3)]],
                   device float *ref_out [[buffer(4)]], device float *dis_out [[buffer(5)]],
                   constant uint4 &dims [[buffer(6)]], constant uint4 &cfg [[buffer(7)]],
                   constant uint4 &bpc4 [[buffer(8)]], uint2 gid [[thread_position_in_grid]])
{
    const int out_w = (int)dims.x;
    const int out_h = (int)dims.y;
    const int in_w = (int)dims.z;
    const int in_h = (int)dims.w;
    const int scale = (int)cfg.x;
    const uint raw_stride = cfg.y;
    const uint f_stride = cfg.z;
    const uint out_stride = cfg.w;
    const uint bpc = bpc4.x;

    const int gx = (int)gid.x;
    const int gy = (int)gid.y;
    if (gx >= out_w || gy >= out_h) {
        return;
    }

    const int fw = fvif_fw(scale);
    const int hfw = fw / 2;
    constant float *coeff = fvif_coeffs(scale);
    const int in_x = 2 * gx;
    const int in_y = 2 * gy;
    const bool is_raw = (scale == 1);

    float acc_ref = 0.0f, acc_dis = 0.0f;
    for (int kj = 0; kj < fw; ++kj) {
        const float c_j = coeff[kj];
        const int px = fvif_mirror(in_x - hfw + kj, in_w);
        float v_ref = 0.0f, v_dis = 0.0f;
        for (int ki = 0; ki < fw; ++ki) {
            const float c_i = coeff[ki];
            const int py = fvif_mirror(in_y - hfw + ki, in_h);
            float r, d;
            if (is_raw) {
                r = fvif_read_raw(ref_raw, raw_stride, py, px, bpc);
                d = fvif_read_raw(dis_raw, raw_stride, py, px, bpc);
            } else {
                r = ref_f[(uint)py * f_stride + (uint)px];
                d = dis_f[(uint)py * f_stride + (uint)px];
            }
            v_ref += c_i * r;
            v_dis += c_i * d;
        }
        acc_ref += c_j * v_ref;
        acc_dis += c_j * v_dis;
    }

    ref_out[(uint)gy * out_stride + (uint)gx] = acc_ref;
    dis_out[(uint)gy * out_stride + (uint)gx] = acc_dis;
}
