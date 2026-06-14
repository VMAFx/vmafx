/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Metal compute kernel for the ciede2000 feature extractor
 *  (Metal parity sweep — ciede twin). Per-pixel translation of the
 *  CUDA reference core/src/feature/cuda/integer_ciede/ciede_score.cu
 *  and its SYCL twin core/src/feature/sycl/integer_ciede_sycl.cpp,
 *  both of which mirror the per-pixel portion of the CPU reference
 *  core/src/feature/ciede.c (.name="ciede", feature "ciede2000").
 *
 *  Algorithm (must match the float-precision GPU contract — the
 *  CUDA / SYCL twins both measured places=4 on real hardware):
 *    1. Per pixel: YUV (BT.709 limited range) -> linear RGB ->
 *       XYZ -> L*a*b*, for both ref and dis.
 *    2. ΔE2000 between the two Lab colours (k_l=0.65, k_c=1, k_h=4).
 *    3. Reduce per-threadgroup ΔE sum into a single float partial
 *       written to partials[bid.y * grid_w + bid.x].
 *    4. Host (collect): total = Σ partials (double accumulate),
 *       mean_de = total / (W * H),
 *       score = 45 - 20·log10(mean_de).
 *
 *  Chroma subsampling is resolved host-side: integer_ciede_metal.mm
 *  nearest-neighbour upscales U and V to luma resolution before
 *  upload (mirrors ciede.c::scale_chroma_planes / the SYCL twin's
 *  upscale_plane), so all six input planes here are addressed at
 *  luma resolution with a single per-plane row stride.
 *
 *  No 64-bit MSL atomics: per-threadgroup partial sum (simd_sum ->
 *  threadgroup array -> one float per group), host sums in double.
 *  This is the float_psnr.metal reduction pattern (ADR-0421).
 *
 *  Buffer bindings (8bpc kernel; host must match integer_ciede_metal.mm):
 *   [[buffer(0)]] ref_y    — const uchar *  (luma-res Y, row stride = strides.x)
 *   [[buffer(1)]] ref_u    — const uchar *  (luma-res U, row stride = strides.x)
 *   [[buffer(2)]] ref_v    — const uchar *  (luma-res V, row stride = strides.x)
 *   [[buffer(3)]] dis_y    — const uchar *  (luma-res Y, row stride = strides.x)
 *   [[buffer(4)]] dis_u    — const uchar *  (luma-res U, row stride = strides.x)
 *   [[buffer(5)]] dis_v    — const uchar *  (luma-res V, row stride = strides.x)
 *   [[buffer(6)]] partials — float *        (grid_w × grid_h floats)
 *   [[buffer(7)]] strides  — uint2          (.x=row_stride_bytes, .y=bpc)
 *   [[buffer(8)]] dim      — uint2          (width, height)
 *
 *  Buffer bindings (16bpc kernel): identical, the same uint2 strides
 *  carries bpc in .y and the kernel reads ushort instead of uchar.
 */

#include <metal_stdlib>
using namespace metal;

/* ------------------------------------------------------------------ */
/*  Shared device-side colour-difference math (mirrors ciede_score.cu) */
/* ------------------------------------------------------------------ */

static inline float ciede_pow_pos_2_4(float x)
{
    return pow(x, 2.4f);
}

static inline float ciede_srgb_to_linear(float c)
{
    if (c > 10.0f / 255.0f) {
        const float A = 0.055f;
        const float D = 1.0f / 1.055f;
        return ciede_pow_pos_2_4((c + A) * D);
    }
    return c / 12.92f;
}

static inline float ciede_xyz_to_lab_map(float t)
{
    if (t > 0.008856f) {
        return cbrt(t);
    }
    return 7.787f * t + (16.0f / 116.0f);
}

static inline void ciede_yuv_to_lab(float y_lim, float u_lim, float v_lim, uint bpc,
                                    thread float *L, thread float *A, thread float *B)
{
    float scale = 1.0f;
    if (bpc == 10u) {
        scale = 4.0f;
    } else if (bpc == 12u) {
        scale = 16.0f;
    } else if (bpc == 16u) {
        scale = 256.0f;
    }
    float y = (y_lim - 16.0f * scale) * (1.0f / (219.0f * scale));
    float u = (u_lim - 128.0f * scale) * (1.0f / (224.0f * scale));
    float v = (v_lim - 128.0f * scale) * (1.0f / (224.0f * scale));
    float r = y + 1.28033f * v;
    float g = y - 0.21482f * u - 0.38059f * v;
    float b = y + 2.12798f * u;
    r = ciede_srgb_to_linear(r);
    g = ciede_srgb_to_linear(g);
    b = ciede_srgb_to_linear(b);
    float x = r * 0.4124564390896921f + g * 0.357576077643909f + b * 0.18043748326639894f;
    float yy = r * 0.21267285140562248f + g * 0.715152155287818f + b * 0.07217499330655958f;
    float z = r * 0.019333895582329317f + g * 0.119192025881303f + b * 0.9503040785363677f;
    x *= 1.0f / 0.95047f;
    z *= 1.0f / 1.08883f;
    float lx = ciede_xyz_to_lab_map(x);
    float ly = ciede_xyz_to_lab_map(yy);
    float lz = ciede_xyz_to_lab_map(z);
    *L = 116.0f * ly - 16.0f;
    *A = 500.0f * (lx - ly);
    *B = 200.0f * (ly - lz);
}

static inline float ciede_get_h_prime(float b, float a)
{
    if (b == 0.0f && a == 0.0f) {
        return 0.0f;
    }
    float h = atan2(b, a);
    if (h < 0.0f) {
        h += 6.283185307179586f;
    }
    return h * 180.0f / 3.141592653589793f;
}

static inline float ciede_get_delta_h_prime(float c1, float c2, float h1, float h2)
{
    if (c1 * c2 == 0.0f) {
        return 0.0f;
    }
    float diff = h2 - h1;
    if (fabs(diff) <= 180.0f) {
        return diff * 3.141592653589793f / 180.0f;
    }
    if (diff > 180.0f) {
        return (diff - 360.0f) * 3.141592653589793f / 180.0f;
    }
    return (diff + 360.0f) * 3.141592653589793f / 180.0f;
}

static inline float ciede_get_upcase_h_bar_prime(float h1, float h2)
{
    float diff = fabs(h1 - h2);
    if (diff > 180.0f) {
        return ((h1 + h2 + 360.0f) / 2.0f) * 3.141592653589793f / 180.0f;
    }
    return ((h1 + h2) / 2.0f) * 3.141592653589793f / 180.0f;
}

static inline float ciede_get_upcase_t(float h_bar)
{
    return 1.0f - 0.17f * cos(h_bar - 3.141592653589793f / 6.0f) + 0.24f * cos(2.0f * h_bar) +
           0.32f * cos(3.0f * h_bar + 3.141592653589793f / 30.0f) -
           0.20f * cos(4.0f * h_bar - 63.0f * 3.141592653589793f / 180.0f);
}

static inline float ciede_get_r_sub_t(float c_bar, float h_bar)
{
    float exponent = -pow((h_bar * 180.0f / 3.141592653589793f - 275.0f) / 25.0f, 2.0f);
    float c7 = pow(c_bar, 7.0f);
    float r_c = 2.0f * sqrt(c7 / (c7 + pow(25.0f, 7.0f)));
    return -sin(60.0f * 3.141592653589793f / 180.0f * exp(exponent)) * r_c;
}

static inline float ciede2000_dev(float l1, float a1, float b1, float l2, float a2, float b2)
{
    const float k_l = 0.65f;
    const float k_c = 1.0f;
    const float k_h = 4.0f;
    float dl_p = l2 - l1;
    float l_bar = 0.5f * (l1 + l2);
    float c1 = sqrt(a1 * a1 + b1 * b1);
    float c2 = sqrt(a2 * a2 + b2 * b2);
    float c_bar = 0.5f * (c1 + c2);
    float c_bar_7 = pow(c_bar, 7.0f);
    float g_factor = 1.0f - sqrt(c_bar_7 / (c_bar_7 + pow(25.0f, 7.0f)));
    float a1_p = a1 + 0.5f * a1 * g_factor;
    float a2_p = a2 + 0.5f * a2 * g_factor;
    float c1_p = sqrt(a1_p * a1_p + b1 * b1);
    float c2_p = sqrt(a2_p * a2_p + b2 * b2);
    float c_bar_p = 0.5f * (c1_p + c2_p);
    float dc_p = c2_p - c1_p;
    float dl2 = (l_bar - 50.0f) * (l_bar - 50.0f);
    float s_l = 1.0f + (0.015f * dl2) / sqrt(20.0f + dl2);
    float s_c = 1.0f + 0.045f * c_bar_p;
    float h1_p = ciede_get_h_prime(b1, a1_p);
    float h2_p = ciede_get_h_prime(b2, a2_p);
    float dh_p = ciede_get_delta_h_prime(c1, c2, h1_p, h2_p);
    float dH_p = 2.0f * sqrt(c1_p * c2_p) * sin(dh_p / 2.0f);
    float H_bar_p = ciede_get_upcase_h_bar_prime(h1_p, h2_p);
    float t_term = ciede_get_upcase_t(H_bar_p);
    float s_h = 1.0f + 0.015f * c_bar_p * t_term;
    float r_t = ciede_get_r_sub_t(c_bar_p, H_bar_p);
    float lightness = dl_p / (k_l * s_l);
    float chroma = dc_p / (k_c * s_c);
    float hue = dH_p / (k_h * s_h);
    return sqrt(lightness * lightness + chroma * chroma + hue * hue + r_t * chroma * hue);
}

/* ------------------------------------------------------------------ */
/*  Per-threadgroup reduction helper (float_psnr.metal pattern)        */
/* ------------------------------------------------------------------ */
static inline void ciede_reduce_and_store(float my_de, device float *partials, uint2 bid,
                                          uint2 grid_groups, uint lid, uint simd_lane, uint simd_id,
                                          uint simd_count,
                                          threadgroup float *simd_partials)
{
    const float lane_sum = simd_sum(my_de);
    if (simd_lane == 0) {
        simd_partials[simd_id] = lane_sum;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0) {
        float group_sum = 0.0f;
        for (uint i = 0; i < simd_count; ++i) {
            group_sum += simd_partials[i];
        }
        partials[bid.y * grid_groups.x + bid.x] = group_sum;
    }
}

/* ------------------------------------------------------------------ */
/*  8 bpc kernel                                                        */
/* ------------------------------------------------------------------ */
kernel void integer_ciede_kernel_8bpc(
    const device uchar  *ref_y    [[buffer(0)]],
    const device uchar  *ref_u    [[buffer(1)]],
    const device uchar  *ref_v    [[buffer(2)]],
    const device uchar  *dis_y    [[buffer(3)]],
    const device uchar  *dis_u    [[buffer(4)]],
    const device uchar  *dis_v    [[buffer(5)]],
    device       float  *partials [[buffer(6)]],
    constant     uint2  &strides  [[buffer(7)]],
    constant     uint2  &dim      [[buffer(8)]],
    uint2  gid         [[thread_position_in_grid]],
    uint2  bid         [[threadgroup_position_in_grid]],
    uint2  grid_groups [[threadgroups_per_grid]],
    uint   lid         [[thread_index_in_threadgroup]],
    uint   simd_lane   [[thread_index_in_simdgroup]],
    uint   simd_id     [[simdgroup_index_in_threadgroup]],
    uint   simd_count  [[simdgroups_per_threadgroup]])
{
    const int width  = (int)dim.x;
    const int height = (int)dim.y;
    const uint row    = strides.x;
    const uint bpc    = strides.y;

    float my_de = 0.0f;
    if ((int)gid.x < width && (int)gid.y < height) {
        const uint off = gid.y * row + gid.x;
        float l1, a1, b1, l2, a2, b2;
        ciede_yuv_to_lab((float)ref_y[off], (float)ref_u[off], (float)ref_v[off], bpc, &l1, &a1, &b1);
        ciede_yuv_to_lab((float)dis_y[off], (float)dis_u[off], (float)dis_v[off], bpc, &l2, &a2, &b2);
        my_de = ciede2000_dev(l1, a1, b1, l2, a2, b2);
    }

    threadgroup float simd_partials[8];
    ciede_reduce_and_store(my_de, partials, bid, grid_groups, lid, simd_lane, simd_id, simd_count,
                           simd_partials);
}

/* ------------------------------------------------------------------ */
/*  16 bpc kernel                                                       */
/* ------------------------------------------------------------------ */
kernel void integer_ciede_kernel_16bpc(
    const device uchar  *ref_y    [[buffer(0)]],
    const device uchar  *ref_u    [[buffer(1)]],
    const device uchar  *ref_v    [[buffer(2)]],
    const device uchar  *dis_y    [[buffer(3)]],
    const device uchar  *dis_u    [[buffer(4)]],
    const device uchar  *dis_v    [[buffer(5)]],
    device       float  *partials [[buffer(6)]],
    constant     uint2  &strides  [[buffer(7)]],
    constant     uint2  &dim      [[buffer(8)]],
    uint2  gid         [[thread_position_in_grid]],
    uint2  bid         [[threadgroup_position_in_grid]],
    uint2  grid_groups [[threadgroups_per_grid]],
    uint   lid         [[thread_index_in_threadgroup]],
    uint   simd_lane   [[thread_index_in_simdgroup]],
    uint   simd_id     [[simdgroup_index_in_threadgroup]],
    uint   simd_count  [[simdgroups_per_threadgroup]])
{
    const int width  = (int)dim.x;
    const int height = (int)dim.y;
    /* row stride is in bytes; element step for ushort is row/2 */
    const uint row_elems = strides.x >> 1u;
    const uint bpc       = strides.y;

    float my_de = 0.0f;
    if ((int)gid.x < width && (int)gid.y < height) {
        const device ushort *ry = (const device ushort *)ref_y;
        const device ushort *ru = (const device ushort *)ref_u;
        const device ushort *rv = (const device ushort *)ref_v;
        const device ushort *dy = (const device ushort *)dis_y;
        const device ushort *du = (const device ushort *)dis_u;
        const device ushort *dv = (const device ushort *)dis_v;
        const uint off = gid.y * row_elems + gid.x;
        float l1, a1, b1, l2, a2, b2;
        ciede_yuv_to_lab((float)ry[off], (float)ru[off], (float)rv[off], bpc, &l1, &a1, &b1);
        ciede_yuv_to_lab((float)dy[off], (float)du[off], (float)dv[off], bpc, &l2, &a2, &b2);
        my_de = ciede2000_dev(l1, a1, b1, l2, a2, b2);
    }

    threadgroup float simd_partials[8];
    ciede_reduce_and_store(my_de, partials, bid, grid_groups, lid, simd_lane, simd_id, simd_count,
                           simd_partials);
}
