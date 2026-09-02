/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Metal compute kernels for the CAMBI banding-detection feature
 *  extractor (feature name "cambi"; Metal twin "integer_cambi_metal").
 *
 *  Metal port of core/src/feature/cuda/integer_cambi/cambi_score.cu
 *  (T3-15 / ADR-0360) following the same Strategy II hybrid design
 *  (ADR-0205 precedent): the embarrassingly parallel integer stages run
 *  on the GPU here, while the precision-sensitive sliding-histogram
 *  calculate_c_values pass and the top-K spatial pooling run on the host
 *  CPU via the shared wrappers in core/src/feature/cambi_internal.h.
 *  This keeps the emitted CAMBI score bit-for-bit identical to the CPU
 *  `cambi` extractor (places=4 / ULP=0, ADR-0214 gate) at no extra
 *  development risk from a fully-on-GPU histogram pass.
 *
 *  GPU kernel inventory (all integer, all bit-exact with cambi.c):
 *
 *    cambi_mask_kernel    -- per-pixel derivative (pixel == right AND ==
 *        below; edges count as equal) summed over a 7x7 box, thresholded
 *        against mask_index. One thread per output pixel; the 7x7 window
 *        re-reads the derivative from global memory with the exact same
 *        clamping as cambi.c::get_derivative_data_for_row +
 *        compute_mask_row. Output mask: 1 = flat (banding candidate),
 *        0 = edge. Matches cambi.c::get_spatial_mask_for_index.
 *
 *    cambi_decimate_kernel -- strict 2x stride-2 subsample of a uint16
 *        luma buffer. One thread per output pixel. Bit-exact with
 *        cambi.c::decimate.
 *
 *    cambi_filter_mode_kernel -- separable 3-tap mode filter, horizontal
 *        pass first then vertical, each in a separate kernel launch
 *        (axis == 0 -> H, axis == 1 -> V). One thread per output pixel.
 *        Bit-exact with cambi.c::filter_mode / mode3.
 *
 *  All buffers are flat uint16_t device arrays (Shared storage on Apple
 *  unified memory); the host .mm wrapper copies from/to VmafPicture
 *  layout via plain memcpy (no DtoH copy needed — [buffer contents]).
 *
 *  No 64-bit MSL atomics are used: there is no GPU-side reduction here.
 *  Every GPU kernel writes a dense per-pixel output; the host performs
 *  the histogram + pooling reductions in the exact CPU code path.
 *
 *  Buffer bindings (all kernels):
 *   [[buffer(0)]] in/src/image  -- const device ushort *
 *   [[buffer(1)]] out/dst/mask  -- device ushort *
 *   [[buffer(2)]] params        -- uint4 / uint2 (see per-kernel docs)
 */

#include <metal_stdlib>
using namespace metal;

/* ------------------------------------------------------------------ */
/*  Kernel 1: Spatial mask (7x7 zero-derivative box sum + threshold)    */
/*                                                                       */
/*  Matches cambi.c::get_spatial_mask_for_index.                         */
/*    zero_deriv(x,y) = ( x==W-1 || img[y][x]==img[y][x+1] ) &&          */
/*                      ( y==H-1 || img[y][x]==img[y+1][x] )             */
/*    box_sum(x,y)    = sum over the 7x7 window centred at (x,y),        */
/*                      with each tap clamped to the image bounds.       */
/*    mask(x,y)       = (box_sum > mask_index) ? 1 : 0                   */
/*                                                                       */
/*  cambi.c builds the box sum with a clamped-edge SAT (the DP table     */
/*  zero-pads out-of-frame derivative contributions). Because the        */
/*  derivative of an out-of-frame pixel is 0 in the SAT recurrence (the  */
/*  DP table starts at zero and only accumulates valid rows/cols), the   */
/*  box sum is exactly the count of in-frame zero-derivative pixels      */
/*  inside the 7x7 window. We reproduce that here by clamping each tap   */
/*  to [0,W-1] x [0,H-1] is INCORRECT (would double-count the edge);     */
/*  instead we skip taps that fall outside the frame, matching the       */
/*  zero-pad semantics of the SAT exactly.                               */
/*                                                                       */
/*  params: .x = W, .y = H, .z = stride_words, .w = mask_index           */
/* ------------------------------------------------------------------ */
static inline ushort zero_deriv_at(const device ushort *image, int x, int y, int W, int H,
                                   uint stride_words)
{
    const ushort p = image[(uint)y * stride_words + (uint)x];
    const int xr = (x == W - 1) ? x : x + 1;
    const int yb = (y == H - 1) ? y : y + 1;
    const ushort r = image[(uint)y * stride_words + (uint)xr];
    const ushort b = image[(uint)yb * stride_words + (uint)x];
    const bool eq_r = (x == W - 1) || (p == r);
    const bool eq_b = (y == H - 1) || (p == b);
    return (eq_r && eq_b) ? 1u : 0u;
}

kernel void cambi_mask_kernel(const device ushort *image [[buffer(0)]],
                              device ushort *mask [[buffer(1)]],
                              constant uint4 &params [[buffer(2)]],
                              uint2 gid [[thread_position_in_grid]])
{
    const int W = (int)params.x;
    const int H = (int)params.y;
    const uint stride_words = params.z;
    const uint mask_index = params.w;

    const int x = (int)gid.x;
    const int y = (int)gid.y;
    if (x >= W || y >= H) {
        return;
    }

    /* 7x7 box, half-width 3 (MASK_FILTER_SIZE == 7). Taps that fall
     * outside the frame contribute 0 (zero-pad, matching the CPU SAT). */
    const int pad = 3;
    uint box_sum = 0u;
    for (int dy = -pad; dy <= pad; ++dy) {
        const int yy = y + dy;
        if (yy < 0 || yy >= H) {
            continue;
        }
        for (int dx = -pad; dx <= pad; ++dx) {
            const int xx = x + dx;
            if (xx < 0 || xx >= W) {
                continue;
            }
            box_sum += (uint)zero_deriv_at(image, xx, yy, W, H, stride_words);
        }
    }

    mask[(uint)y * stride_words + (uint)x] = (box_sum > mask_index) ? 1u : 0u;
}

/* ------------------------------------------------------------------ */
/*  Kernel 2: 2x decimate (strict even-pixel subsample, no filter)      */
/*                                                                       */
/*  Matches cambi.c::decimate:                                           */
/*    dst[y][x] = src[2y][2x]                                            */
/*                                                                       */
/*  params: .x = out_W, .y = out_H, .z = src_stride_words,               */
/*          .w = dst_stride_words                                        */
/* ------------------------------------------------------------------ */
kernel void cambi_decimate_kernel(const device ushort *src [[buffer(0)]],
                                  device ushort *dst [[buffer(1)]],
                                  constant uint4 &params [[buffer(2)]],
                                  uint2 gid [[thread_position_in_grid]])
{
    const uint out_w = params.x;
    const uint out_h = params.y;
    const uint src_stride_words = params.z;
    const uint dst_stride_words = params.w;

    const uint x = gid.x;
    const uint y = gid.y;
    if (x >= out_w || y >= out_h) {
        return;
    }

    dst[y * dst_stride_words + x] = src[(y * 2u) * src_stride_words + x * 2u];
}

/* ------------------------------------------------------------------ */
/*  Kernel 3: separable 3-tap mode filter                               */
/*                                                                       */
/*  axis == 0 (H): out[y][x] = mode3(in[y][x-1], in[y][x], in[y][x+1])  */
/*  axis == 1 (V): out[y][x] = mode3(in[y-1][x], in[y][x], in[y+1][x])  */
/*  with clamped borders. Matches cambi.c::filter_mode / mode3.          */
/*                                                                       */
/*  params: .x = W, .y = H, .z = stride_words, .w = axis (0 or 1)        */
/* ------------------------------------------------------------------ */
static inline ushort mode3_dev(ushort a, ushort b, ushort c)
{
    /* Two equal -> that value. All distinct -> min of the three.
     * (cambi.c::mode3 returns `a` when a==b or a==c, else `b` when
     *  b==c, else min3.) */
    if (a == b || a == c) {
        return a;
    }
    if (b == c) {
        return b;
    }
    return (a < b) ? ((a < c) ? a : c) : ((b < c) ? b : c);
}

kernel void cambi_filter_mode_kernel(const device ushort *in [[buffer(0)]],
                                     device ushort *out [[buffer(1)]],
                                     constant uint4 &params [[buffer(2)]],
                                     uint2 gid [[thread_position_in_grid]])
{
    const int W = (int)params.x;
    const int H = (int)params.y;
    const uint stride_words = params.z;
    const int axis = (int)params.w;

    const int x = (int)gid.x;
    const int y = (int)gid.y;
    if (x >= W || y >= H) {
        return;
    }

    ushort a, b, c;
    if (axis == 0) {
        const int xl = (x > 0) ? x - 1 : 0;
        const int xr = (x < W - 1) ? x + 1 : W - 1;
        a = in[(uint)y * stride_words + (uint)xl];
        b = in[(uint)y * stride_words + (uint)x];
        c = in[(uint)y * stride_words + (uint)xr];
    } else {
        const int yu = (y > 0) ? y - 1 : 0;
        const int yd = (y < H - 1) ? y + 1 : H - 1;
        a = in[(uint)yu * stride_words + (uint)x];
        b = in[(uint)y * stride_words + (uint)x];
        c = in[(uint)yd * stride_words + (uint)x];
    }
    out[(uint)y * stride_words + (uint)x] = mode3_dev(a, b, c);
}
