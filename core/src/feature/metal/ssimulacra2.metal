/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Metal compute kernels for the ssimulacra2 feature (feature
 *  "ssimulacra2"). Metal twin of the CUDA kernels under
 *  core/src/feature/cuda/ssimulacra2/ (ssimulacra2_mul.cu +
 *  ssimulacra2_blur.cu) and the SYCL twin; both mirror the libjxl
 *  scalar reference ported in core/src/feature/ssimulacra2.c.
 *
 *  ARCHITECTURE (must match ssimulacra2_cuda.c for places=4 parity):
 *
 *  The GPU only runs two operations — the elementwise 3-plane multiply
 *  and the separable FastGaussian 3-pole recursive IIR blur. Everything
 *  else stays on the host (in ssimulacra2_metal.mm):
 *
 *    - YUV -> linear-RGB (host: deterministic LUT sRGB EOTF, ADR-0164)
 *    - linear-RGB -> XYB (host: the GPU's float cbrt differs from libm
 *      by tens of ULP — that drift cascades through the IIR + 108-weight
 *      pool to a ~1.5e-2 pooled-score drift; ADR-0201 keeps XYB on host)
 *    - per-pixel SSIM combine + EdgeDiff (host: double precision; MSL
 *      has no double, and the `1 - num_m * num_s / denom_s` divide needs
 *      double promotion at the divide site)
 *    - 2x2 box downsample of the linear-RGB pyramid (host)
 *    - 108 weighted-norm polynomial pool (host)
 *
 *  GPU kernels:
 *
 *  1. `ssimulacra2_mul3` — elementwise out[i] = a[i] * b[i] over a
 *     contiguous 3-plane buffer (X | Y | B). Pure multiply; no
 *     reduction. Mirrors multiply_3plane in ssimulacra2.c.
 *
 *  2. `ssimulacra2_blur_h3` / `ssimulacra2_blur_v3` — the separable
 *     3-pole recursive IIR (Charalampidis 2016, k={1,3,5}, sigma=1.5,
 *     zero-padded boundaries). The IIR is sequential along the scan
 *     axis; separability lets one thread own one row (H) / one column
 *     (V). gridDim.z = 3 fuses all three XYB planes into one launch.
 *     Bit-identical control flow with fast_gaussian_1d in
 *     ssimulacra2.c.
 *
 *  FMA-CONTRACTION CONTROL (parity-critical): the CPU writes the IIR
 *  recurrence `o = n2*sum - d1*prev1 - prev2` as separate FMUL/FSUB ops
 *  under -ffp-contract=off, and the CUDA fatbin is compiled with
 *  --fmad=false. The Metal `metal` compiler defaults to -ffast-math
 *  (contraction ON) and the build does not pass -fno-fast-math (the
 *  per-feature .air custom_target in core/src/metal/meson.build uses a
 *  bare `metal -c`). To keep the IIR pole-tracking from compounding an
 *  FMA-rounding delta across the radius and the 6-scale pyramid, each
 *  multiply result is forced through `nc()` — a no-contract barrier that
 *  routes the value through a `thread volatile` slot so the compiler
 *  cannot fuse the following subtract into an FMA. This is the MSL
 *  source-level equivalent of CUDA's --fmad=false. See ADR-0201
 *  (precision investigation) and the CUDA twin's block comment.
 */

#include <metal_stdlib>
using namespace metal;

/* No-contract barrier: routing a float through a thread-local volatile
 * slot defeats the -ffast-math FP contraction the Metal compiler would
 * otherwise apply, so `nc(a) - b` stays a separate FMUL then FSUB
 * (matches CPU -ffp-contract=off / CUDA --fmad=false). Costs one extra
 * round-trip per use; only applied on the IIR's per-pole products. */
inline float nc(float v)
{
    thread volatile float t = v;
    return t;
}

/* ------------------------------------------------------------------ */
/*  Kernel 1: ssimulacra2_mul3 — elementwise 3-plane multiply          */
/*                                                                      */
/*  Mirrors multiply_3plane in ssimulacra2.c: out[i] = a[i] * b[i]      */
/*  over a contiguous 3-plane buffer. The channel offset is            */
/*  c * plane_stride (plane_stride == full_w * full_h, kept constant    */
/*  across pyramid scales for layout consistency with the host).        */
/*                                                                      */
/*  Buffer bindings:                                                    */
/*   [[buffer(0)]] a            — const float * (3 x plane_stride)      */
/*   [[buffer(1)]] b            — const float * (3 x plane_stride)      */
/*   [[buffer(2)]] out          — float *       (3 x plane_stride)      */
/*   [[buffer(3)]] params       — uint4 (.x=width, .y=height,           */
/*                                        .z=plane_count, .w=plane_str) */
/*  Grid: ceil(w/16) x ceil(h/8), threads 16x8.                        */
/* ------------------------------------------------------------------ */
kernel void ssimulacra2_mul3(
    const device float  *a      [[buffer(0)]],
    const device float  *b      [[buffer(1)]],
    device       float  *out    [[buffer(2)]],
    constant     uint4  &params [[buffer(3)]],
    uint2  gid [[thread_position_in_grid]])
{
    const uint width        = params.x;
    const uint height        = params.y;
    const uint plane_count  = params.z;
    const uint plane_stride = params.w;

    const uint x = gid.x;
    const uint y = gid.y;
    if (x >= width || y >= height) { return; }

    const uint base = y * width + x;
    for (uint c = 0u; c < plane_count; ++c) {
        const uint idx = base + c * plane_stride;
        out[idx] = a[idx] * b[idx];
    }
}

/* ------------------------------------------------------------------ */
/*  Kernel 2: ssimulacra2_blur_h3 — fused 3-channel horizontal IIR      */
/*                                                                      */
/*  One thread per row; gridDim.z = 3 selects the XYB plane. Scans      */
/*  left-to-right. Bit-identical control flow with fast_gaussian_1d in  */
/*  ssimulacra2.c (zero-padded boundaries, k={1,3,5} accumulators).     */
/*                                                                      */
/*  Buffer bindings:                                                    */
/*   [[buffer(0)]] in_buf  — const float * (3 x plane_stride)          */
/*   [[buffer(1)]] out_buf — float *       (3 x plane_stride)          */
/*   [[buffer(2)]] params  — uint4 (.x=width, .y=height,               */
/*                                   .z=radius, .w=plane_stride)        */
/*   [[buffer(3)]] coeffs  — float4x2-ish packed as two float4:        */
/*                            n2 = (n2_0, n2_1, n2_2, 0),              */
/*                            d1 = (d1_0, d1_1, d1_2, 0)              */
/*  Grid: ceil(height/64) x 1 x 3, threads 64x1x1.                     */
/* ------------------------------------------------------------------ */
kernel void ssimulacra2_blur_h3(
    const device float  *in_buf  [[buffer(0)]],
    device       float  *out_buf [[buffer(1)]],
    constant     uint4  &params  [[buffer(2)]],
    constant     float4 &n2v     [[buffer(3)]],
    constant     float4 &d1v     [[buffer(4)]],
    uint3  gid [[thread_position_in_grid]])
{
    const uint width        = params.x;
    const uint height        = params.y;
    const int  radius       = (int)params.z;
    const uint plane_stride = params.w;

    const uint c   = gid.z;
    const uint row = gid.x;
    if (row >= height) { return; }

    const int xsize = (int)width;
    const int N     = radius;

    const float n2_0 = n2v.x, n2_1 = n2v.y, n2_2 = n2v.z;
    const float d1_0 = d1v.x, d1_1 = d1v.y, d1_2 = d1v.z;

    float prev1_0 = 0.0f, prev1_1 = 0.0f, prev1_2 = 0.0f;
    float prev2_0 = 0.0f, prev2_1 = 0.0f, prev2_2 = 0.0f;

    const uint in_base  = c * plane_stride + row * width;
    const uint out_base = c * plane_stride + row * width;

    for (int n = -N + 1; n < xsize; ++n) {
        const int left  = n - N - 1;
        const int right = n + N - 1;
        const float lv = (left  >= 0)     ? in_buf[in_base + (uint)left]  : 0.0f;
        const float rv = (right < xsize)  ? in_buf[in_base + (uint)right] : 0.0f;
        const float sum = lv + rv;

        /* o = n2*sum - d1*prev1 - prev2, kept FMA-free via nc(). */
        const float o0 = nc(n2_0 * sum) - nc(d1_0 * prev1_0) - prev2_0;
        const float o1 = nc(n2_1 * sum) - nc(d1_1 * prev1_1) - prev2_1;
        const float o2 = nc(n2_2 * sum) - nc(d1_2 * prev1_2) - prev2_2;
        prev2_0 = prev1_0;
        prev2_1 = prev1_1;
        prev2_2 = prev1_2;
        prev1_0 = o0;
        prev1_1 = o1;
        prev1_2 = o2;

        if (n >= 0) {
            out_buf[out_base + (uint)n] = o0 + o1 + o2;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Kernel 3: ssimulacra2_blur_v3 — fused 3-channel vertical IIR        */
/*                                                                      */
/*  One thread per column; gridDim.z = 3 selects the XYB plane. Scans   */
/*  top-to-bottom over the row-major H-pass output. Bit-identical       */
/*  control flow with the vertical pass of blur_plane in ssimulacra2.c. */
/*                                                                      */
/*  Reads are stride-`width` (non-coalesced) — the CUDA twin adds a     */
/*  transpose for coalescing, but on Apple unified memory the simpler   */
/*  direct-stride read keeps the kernel bit-identical to the CPU scan   */
/*  order without the extra transpose launch. Correctness, not          */
/*  throughput, is the parity gate's concern.                           */
/*                                                                      */
/*  Buffer bindings: identical layout to ssimulacra2_blur_h3.           */
/*  Grid: ceil(width/64) x 1 x 3, threads 64x1x1.                       */
/* ------------------------------------------------------------------ */
kernel void ssimulacra2_blur_v3(
    const device float  *in_buf  [[buffer(0)]],
    device       float  *out_buf [[buffer(1)]],
    constant     uint4  &params  [[buffer(2)]],
    constant     float4 &n2v     [[buffer(3)]],
    constant     float4 &d1v     [[buffer(4)]],
    uint3  gid [[thread_position_in_grid]])
{
    const uint width        = params.x;
    const uint height        = params.y;
    const int  radius       = (int)params.z;
    const uint plane_stride = params.w;

    const uint c   = gid.z;
    const uint col = gid.x;
    if (col >= width) { return; }

    const int ysize = (int)height;
    const int N     = radius;

    const float n2_0 = n2v.x, n2_1 = n2v.y, n2_2 = n2v.z;
    const float d1_0 = d1v.x, d1_1 = d1v.y, d1_2 = d1v.z;

    float prev1_0 = 0.0f, prev1_1 = 0.0f, prev1_2 = 0.0f;
    float prev2_0 = 0.0f, prev2_1 = 0.0f, prev2_2 = 0.0f;

    const uint base = c * plane_stride;

    for (int n = -N + 1; n < ysize; ++n) {
        const int left  = n - N - 1;
        const int right = n + N - 1;
        const float lv =
            (left  >= 0)    ? in_buf[base + (uint)left  * width + col] : 0.0f;
        const float rv =
            (right < ysize) ? in_buf[base + (uint)right * width + col] : 0.0f;
        const float sum = lv + rv;

        const float o0 = nc(n2_0 * sum) - nc(d1_0 * prev1_0) - prev2_0;
        const float o1 = nc(n2_1 * sum) - nc(d1_1 * prev1_1) - prev2_1;
        const float o2 = nc(n2_2 * sum) - nc(d1_2 * prev1_2) - prev2_2;
        prev2_0 = prev1_0;
        prev2_1 = prev1_1;
        prev2_2 = prev1_2;
        prev1_0 = o0;
        prev1_1 = o1;
        prev1_2 = o2;

        if (n >= 0) {
            out_buf[base + (uint)n * width + col] = o0 + o1 + o2;
        }
    }
}
