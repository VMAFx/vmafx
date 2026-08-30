/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Metal compute kernels for integer_ssim (fixed-point SSIM).
 *  Integer twin of float_ssim.metal — mirrors the float twin's
 *  two-pass separable-Gaussian dispatch structure but swaps the
 *  float-domain 11-tap "valid" convolution for the exact fixed-point
 *  arithmetic of the CPU reference libvmaf/src/feature/integer_ssim.c.
 *
 *  ALGORITHM (must match integer_ssim.c::calc_ssim bit-for-bit):
 *
 *  Gaussian kernel: gaussian_filter_init(sigma=1.5, max_len=5) with
 *  KERNEL_SHIFT=8 (KERNEL_WEIGHT=256) produces a 9-tap integer kernel
 *      G = {2, 9, 28, 55, 68, 55, 28, 9, 2}   (sum == 256), offset = 4.
 *  Both horizontal and vertical passes use the SAME kernel (hkernel ==
 *  vkernel; gaussian_filter_init is called twice with identical args).
 *
 *  Boundary handling is NOT mirror and NOT "valid": the CPU truncates
 *  the kernel at the frame edge and accumulates only in-bounds taps,
 *  so the per-pixel weight `w` shrinks near edges. We replicate the
 *  exact k_min / k_max clamps. Output covers the FULL W x H grid; the
 *  score is sum(per-pixel ssim term) / sum(per-pixel weight).
 *
 *  Pass 0 (horizontal) — integer_ssim_horiz_{8,16}bpc:
 *    For each (x,y) in W x H, accumulate six int64 moments over the
 *    in-bounds horizontal taps:
 *      k_min = max(0, OFFS - x)
 *      k_max = (x+OFFS-W+1 <= 0) ? SZ : SZ - (x+OFFS-W+1)
 *      off   = x - OFFS + k
 *      mux += G[k]*s ; muy += G[k]*d ; x2 += G[k]*s*s ;
 *      xy  += G[k]*s*d ; y2 += G[k]*d*d ; w += G[k]
 *    written to 6 long planes in hbuf (stride W, plane_size = W*H).
 *    Plane layout: [mux | muy | x2 | xy | y2 | w].
 *
 *  Pass 1 (vertical + SSIM combine) — integer_ssim_vert_combine:
 *    For output row oy (== gid.y, full height H), accumulate vertical
 *    taps with the same kernel against the horizontal moments:
 *      y     = oy + OFFS
 *      k_min = (SZ - y - 1 <= 0) ? 0 : SZ - y - 1
 *      k_max = (y+1-H <= 0) ? SZ : SZ - (y+1-H)
 *      srow  = oy + (k - OFFS)
 *      m.* += G[k] * hbuf_plane(srow, x)   (int64)
 *    Then the per-pixel SSIM term, in float (MSL has no double; the CPU
 *    promotes to double here — float32 is the cross-backend parity risk,
 *    bounded by the places=4 / 1e-4 ADR-0214 gate):
 *      w_d = m.w
 *      c1  = samplemax^2 * 0.0001 * w_d^2
 *      c2  = samplemax^2 * 0.0009 * w_d^2
 *      mx2 = mux*mux ; mxy = mux*muy ; my2 = muy*muy
 *      term = m.w * (2*mxy + c1) * (c2 + 2*(xy*w_d - mxy))
 *           / ((mx2 + my2 + c1) * (x2*w_d - mx2 + y2*w_d - my2 + c2))
 *    Per-WG reductions: ssim_parts[wg] += term, ssimw_parts[wg] += m.w.
 *    Host: score = sum(ssim_parts) / sum(ssimw_parts).
 *
 *  No 64-bit MSL atomics: the per-WG reduction sums float `term` and
 *  float `weight` via simd_sum + threadgroup fan-in, one float partial
 *  per WG per channel (same pattern as float_ssim.metal).
 *
 *  Buffer bindings for integer_ssim_horiz_{8,16}bpc:
 *   [[buffer(0)]] ref      — const uchar * (full plane, byte-addressed)
 *   [[buffer(1)]] dis      — const uchar *
 *   [[buffer(2)]] hbuf     — device long * (6 x W x H: mux,muy,x2,xy,y2,w)
 *   [[buffer(3)]] params   — uint4 (.x=W, .y=H, .z=ref_stride_bytes,
 *                                    .w=dis_stride_bytes)
 *  (16bpc adds nothing — strides are byte strides; ushort load via cast.)
 *
 *  Buffer bindings for integer_ssim_vert_combine:
 *   [[buffer(0)]] hbuf       — device long * (6 x W x H from pass 0)
 *   [[buffer(1)]] ssim_parts — device float * (grid_w x grid_h)
 *   [[buffer(2)]] ssimw_parts— device float * (grid_w x grid_h)
 *   [[buffer(3)]] params     — uint4 (.x=W, .y=H, .z=samplemax, .w=0)
 *   [[buffer(4)]] grid_dim   — uint2 (.x=grid_w, .y=grid_h)
 */

#include <metal_stdlib>
using namespace metal;

/* Integer Gaussian kernel — exact output of gaussian_filter_init(1.5, 5)
 * with KERNEL_SHIFT=8 (see integer_ssim.c). 9 taps, sum == 256. */
constant long IG[9] = {2, 9, 28, 55, 68, 55, 28, 9, 2};

#define SSIM_SZ   9
#define SSIM_OFFS 4

#define SSIM_K1 (0.0001f) /* 0.01 * 0.01 */
#define SSIM_K2 (0.0009f) /* 0.03 * 0.03 */

/* ------------------------------------------------------------------ */
/*  Pass 0: horizontal moment accumulation (8 bpc)                      */
/* ------------------------------------------------------------------ */
kernel void integer_ssim_horiz_8bpc(
    const device uchar  *ref    [[buffer(0)]],
    const device uchar  *dis    [[buffer(1)]],
    device       long   *hbuf   [[buffer(2)]],
    constant     uint4  &params [[buffer(3)]],
    uint2  gid [[thread_position_in_grid]])
{
    const int W = (int)params.x;
    const int H = (int)params.y;
    const int ref_stride = (int)params.z;
    const int dis_stride = (int)params.w;

    const int x = (int)gid.x;
    const int y = (int)gid.y;
    if (x >= W || y >= H) { return; }

    long mux = 0, muy = 0, x2 = 0, xy = 0, y2 = 0, w = 0;

    const int k_min = (SSIM_OFFS - x <= 0) ? 0 : (SSIM_OFFS - x);
    const int k_max = (x + SSIM_OFFS - W + 1 <= 0) ? SSIM_SZ
                                                   : SSIM_SZ - (x + SSIM_OFFS - W + 1);

    const device uchar *s_row = ref + y * ref_stride;
    const device uchar *d_row = dis + y * dis_stride;
    for (int k = k_min; k < k_max; ++k) {
        const int off = x - SSIM_OFFS + k;
        const long s = (long)s_row[off];
        const long d = (long)d_row[off];
        const long win = IG[k];
        mux += win * s;
        muy += win * d;
        x2  += win * s * s;
        xy  += win * s * d;
        y2  += win * d * d;
        w   += win;
    }

    const uint plane_size = params.x * params.y;
    const uint dst = (uint)y * params.x + (uint)x;
    hbuf[0u * plane_size + dst] = mux;
    hbuf[1u * plane_size + dst] = muy;
    hbuf[2u * plane_size + dst] = x2;
    hbuf[3u * plane_size + dst] = xy;
    hbuf[4u * plane_size + dst] = y2;
    hbuf[5u * plane_size + dst] = w;
}

/* ------------------------------------------------------------------ */
/*  Pass 0: horizontal moment accumulation (16 bpc)                     */
/* ------------------------------------------------------------------ */
kernel void integer_ssim_horiz_16bpc(
    const device uchar  *ref    [[buffer(0)]],
    const device uchar  *dis    [[buffer(1)]],
    device       long   *hbuf   [[buffer(2)]],
    constant     uint4  &params [[buffer(3)]],
    uint2  gid [[thread_position_in_grid]])
{
    const int W = (int)params.x;
    const int H = (int)params.y;
    const int ref_stride = (int)params.z;
    const int dis_stride = (int)params.w;

    const int x = (int)gid.x;
    const int y = (int)gid.y;
    if (x >= W || y >= H) { return; }

    long mux = 0, muy = 0, x2 = 0, xy = 0, y2 = 0, w = 0;

    const int k_min = (SSIM_OFFS - x <= 0) ? 0 : (SSIM_OFFS - x);
    const int k_max = (x + SSIM_OFFS - W + 1 <= 0) ? SSIM_SZ
                                                   : SSIM_SZ - (x + SSIM_OFFS - W + 1);

    const device ushort *s_row = (const device ushort *)(ref + y * ref_stride);
    const device ushort *d_row = (const device ushort *)(dis + y * dis_stride);
    for (int k = k_min; k < k_max; ++k) {
        const int off = x - SSIM_OFFS + k;
        const long s = (long)s_row[off];
        const long d = (long)d_row[off];
        const long win = IG[k];
        mux += win * s;
        muy += win * d;
        x2  += win * s * s;
        xy  += win * s * d;
        y2  += win * d * d;
        w   += win;
    }

    const uint plane_size = params.x * params.y;
    const uint dst = (uint)y * params.x + (uint)x;
    hbuf[0u * plane_size + dst] = mux;
    hbuf[1u * plane_size + dst] = muy;
    hbuf[2u * plane_size + dst] = x2;
    hbuf[3u * plane_size + dst] = xy;
    hbuf[4u * plane_size + dst] = y2;
    hbuf[5u * plane_size + dst] = w;
}

/* ------------------------------------------------------------------ */
/*  Pass 1: vertical accumulation + per-pixel SSIM + per-WG reduction   */
/* ------------------------------------------------------------------ */
kernel void integer_ssim_vert_combine(
    const device long   *hbuf        [[buffer(0)]],
    device       float  *ssim_parts  [[buffer(1)]],
    device       float  *ssimw_parts [[buffer(2)]],
    constant     uint4  &params      [[buffer(3)]],
    constant     uint2  &grid_dim    [[buffer(4)]],
    uint2  gid        [[thread_position_in_grid]],
    uint2  bid        [[threadgroup_position_in_grid]],
    uint   lid        [[thread_index_in_threadgroup]],
    uint   simd_lane  [[thread_index_in_simdgroup]],
    uint   simd_id    [[simdgroup_index_in_threadgroup]],
    uint   simd_count [[simdgroups_per_threadgroup]])
{
    const int W = (int)params.x;
    const int H = (int)params.y;
    const float samplemax = (float)params.z;

    const int x  = (int)gid.x;
    const int oy = (int)gid.y;

    float term   = 0.0f;
    float weight = 0.0f;

    if (x < W && oy < H) {
        const uint plane_size = params.x * params.y;

        long mux = 0, muy = 0, x2 = 0, xy = 0, y2 = 0, w = 0;

        const int y = oy + SSIM_OFFS;
        const int k_min = (SSIM_SZ - y - 1 <= 0) ? 0 : (SSIM_SZ - y - 1);
        const int k_max = (y + 1 - H <= 0) ? SSIM_SZ : SSIM_SZ - (y + 1 - H);

        for (int k = k_min; k < k_max; ++k) {
            const int srow = oy + (k - SSIM_OFFS);
            const uint base = (uint)srow * params.x + (uint)x;
            const long win = IG[k];
            mux += win * hbuf[0u * plane_size + base];
            muy += win * hbuf[1u * plane_size + base];
            x2  += win * hbuf[2u * plane_size + base];
            xy  += win * hbuf[3u * plane_size + base];
            y2  += win * hbuf[4u * plane_size + base];
            w   += win * hbuf[5u * plane_size + base];
        }

        /* SSIM term — float port of the CPU double-precision combine.
         * The CPU promotes the int64 moments to double before each
         * multiply; we promote to float (MSL has no double). */
        const float w_d = (float)w;
        const float c1  = samplemax * samplemax * SSIM_K1 * w_d * w_d;
        const float c2  = samplemax * samplemax * SSIM_K2 * w_d * w_d;
        const float mx2 = (float)mux * (float)mux;
        const float mxy = (float)mux * (float)muy;
        const float my2 = (float)muy * (float)muy;
        const float num = (float)w * (2.0f * mxy + c1) *
                          (c2 + 2.0f * ((float)xy * w_d - mxy));
        const float den = (mx2 + my2 + c1) *
                          ((float)x2 * w_d - mx2 + (float)y2 * w_d - my2 + c2);
        term   = num / den;
        weight = (float)w;
    }

    /* Per-WG reduction of (term, weight) — two simd_sum passes folded
     * through threadgroup memory. Host sums partials and divides
     * sum(term) / sum(weight). */
    threadgroup float sg_term[8];
    threadgroup float sg_w[8];
    const float lane_term = simd_sum(term);
    const float lane_w    = simd_sum(weight);
    if (simd_lane == 0) {
        sg_term[simd_id] = lane_term;
        sg_w[simd_id]    = lane_w;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0) {
        float gt = 0.0f, gw = 0.0f;
        for (uint i = 0; i < simd_count; ++i) {
            gt += sg_term[i];
            gw += sg_w[i];
        }
        const uint part_idx = bid.y * grid_dim.x + bid.x;
        ssim_parts[part_idx]  = gt;
        ssimw_parts[part_idx] = gw;
    }
}
