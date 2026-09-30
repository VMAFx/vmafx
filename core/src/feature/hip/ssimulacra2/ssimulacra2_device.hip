/**
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  Device module of the HIP ssimulacra2 twin: the whole frame of
 *  ssimulacra2.c::extract on the device (the ADR-1363 chain of
 *  core/src/feature/sycl/ssimulacra2_sycl.cpp, ported to HIP).
 *
 *    ssimulacra2_yuv_to_linear   YUV -> linear RGB from the raw planes
 *    ssimulacra2_xyb             linear RGB -> XYB (MakePositiveXYB folded in)
 *    ssimulacra2_blur_rows/cols  3-pole recursive Gaussian, all three planes;
 *                                the row pass forms ref^2, dis^2, ref*dis
 *    ssimulacra2_combine_*       per-pixel SSIM / edge terms -> six sums per
 *                                channel over a fixed tree of fp32 pairs
 *    ssimulacra2_downsample      2x2 box downsample of the linear-RGB pyramid
 *
 *  Numerical contract: every stage but the combine is bit-identical to the CPU
 *  extractor. That needs contraction off (the pragma below; core/src/meson.build
 *  also passes -ffp-contract=off for this module), products that feed adds in
 *  named temporaries, single-rounded fmaf() in the YUV matrix (ADR-0891 /
 *  ADR-1205), and the shared cube root and sRGB EOTF of ssimulacra2_math.h with
 *  a correctly rounded division (__fdiv_rn; HIP fp32 `/` is correctly rounded
 *  by default, -fhip-fp32-correctly-rounded-divide-sqrt). The CPU evaluates the
 *  per-pixel combine terms in fp64 and adds them one after another; here each
 *  term is an exact fp32 pair (relative error about 2^-44) and the sums follow
 *  the SYCL twin's tree, so the result depends only on the plane size, not on
 *  the device or its wavefront width.
 */

#include <hip/hip_runtime.h>

#include <cstddef>
#include <cstdint>

#include "hip/ssimulacra2_hip.h"

/* Every add, subtract and multiply below rounds on its own: the pair
 * arithmetic is exact only then, and the bit-identical stages need it too. */
#pragma clang fp contract(off)

/* The shared cube root and EOTF are plain C inline functions; compile them for
 * the device as well. The EOTF LUT is a namespace-scope const table, which HIP
 * places in device memory for the device copy of the function. */
#define VMAF_SS2_FDIV(a, b) __fdiv_rn((a), (b))
#pragma clang force_cuda_host_device begin
#include "feature/ssimulacra2_math.h"
#pragma clang force_cuda_host_device end

namespace
{

constexpr float kSs2C2 = 0.0009f; /* ssimulacra2.c kC2 */

/* ------------------------------------------------------------------ */
/* Exact fp32 pairs (the SYCL twin's sycl_exact_fp.h, HIP spelling)    */
/* ------------------------------------------------------------------ */

struct Ff {
    float hi;
    float lo;
};

__device__ inline Ff two_sum(float a, float b)
{
    const float sum = a + b;
    const float b_virtual = sum - a;
    const float a_virtual = sum - b_virtual;
    const float b_error = b - b_virtual;
    const float a_error = a - a_virtual;
    return Ff{sum, a_error + b_error};
}

__device__ inline Ff quick_two_sum(float a, float b)
{
    const float sum = a + b;
    const float rebuilt = sum - a;
    return Ff{sum, b - rebuilt};
}

__device__ inline Ff two_prod(float a, float b)
{
    const float product = a * b;
    return Ff{product, fmaf(a, b, -product)};
}

__device__ inline Ff ff_add(Ff a, Ff b)
{
    const Ff high = two_sum(a.hi, b.hi);
    const Ff low = two_sum(a.lo, b.lo);
    const Ff first = quick_two_sum(high.hi, high.lo + low.hi);
    return quick_two_sum(first.hi, low.lo + first.lo);
}

__device__ inline Ff ff_mul(Ff a, Ff b)
{
    const Ff product = two_prod(a.hi, b.hi);
    const float cross1 = a.hi * b.lo;
    const float cross2 = a.lo * b.hi;
    const float cross = cross1 + cross2;
    return quick_two_sum(product.hi, product.lo + cross);
}

__device__ inline Ff ff_neg(Ff a)
{
    return Ff{-a.hi, -a.lo};
}

/* a / b as a pair; both partial quotients correctly rounded. A non-finite
 * first quotient is returned as is so infinities and NaN reach the caller. */
__device__ inline Ff ff_div(Ff a, Ff b)
{
    const float q1 = __fdiv_rn(a.hi, b.hi);
    if (!isfinite(q1)) {
        return Ff{q1, 0.0f};
    }
    const Ff scaled = ff_mul(b, Ff{q1, 0.0f});
    const Ff residual = ff_add(a, ff_neg(scaled));
    const float q2 = __fdiv_rn(residual.hi, b.hi);
    return quick_two_sum(q1, q2);
}

constexpr Ff kFfZero = {0.0f, 0.0f};
constexpr Ff kFfOne = {1.0f, 0.0f};
constexpr Ff kFfMinusOne = {-1.0f, 0.0f};

/* ------------------------------------------------------------------ */
/* YUV -> linear RGB, XYB, downsample                                  */
/* ------------------------------------------------------------------ */

/* ssimulacra2.c::read_plane coordinate mapping (nearest neighbour). */
__device__ inline unsigned ss2h_map(unsigned v, unsigned plane_dim, unsigned luma_dim)
{
    uint64_t s = v;
    if (plane_dim != luma_dim) {
        s = (plane_dim * 2u == luma_dim) ? (uint64_t)(v >> 1) : (uint64_t)v * plane_dim / luma_dim;
    }
    return (s >= plane_dim) ? plane_dim - 1u : (unsigned)s;
}

__device__ inline float ss2h_sample(const Ss2hYuvArgs &a, unsigned img, unsigned p, unsigned x,
                                    unsigned y)
{
    const unsigned sx = ss2h_map(x, a.plane_w[p], a.width);
    const unsigned sy = ss2h_map(y, a.plane_h[p], a.height);
    const size_t idx = (size_t)sy * a.plane_w[p] + sx;
    if (a.wide) {
        return (float)static_cast<const uint16_t *>(a.plane[img][p])[idx];
    }
    return (float)static_cast<const uint8_t *>(a.plane[img][p])[idx];
}

__device__ inline float ss2h_clampf(float v)
{
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

/* One pixel of ssimulacra2.c::picture_to_linear_rgb for one image. */
__device__ inline void ss2h_yuv_pixel(const Ss2hYuvArgs &a, unsigned img, unsigned x, unsigned y)
{
    const Ss2hYuvCoefficients &k = a.k;
    const float Y = ss2h_sample(a, img, 0u, x, y) * k.inv_peak;
    const float U = ss2h_sample(a, img, 1u, x, y) * k.inv_peak;
    const float V = ss2h_sample(a, img, 2u, x, y) * k.inv_peak;
    const float Yn = (Y - k.y_off) * k.y_scale;
    const float Un = (U - k.c_off) * k.c_scale;
    const float Vn = (V - k.c_off) * k.c_scale;
    /* Single-rounded FMAs in this order (ADR-0891 / ADR-1205). */
    const float R = fmaf(k.cr_r, Vn, Yn);
    const float G0 = fmaf(k.cb_g, Un, Yn);
    const float G = fmaf(k.cr_g, Vn, G0);
    const float B = fmaf(k.cb_b, Un, Yn);
    const size_t plane = (size_t)a.width * a.height;
    const size_t idx = (size_t)y * a.width + x;
    float *out = a.out[img];
    out[idx] = vmaf_ss2_srgb_eotf(ss2h_clampf(R));
    out[plane + idx] = vmaf_ss2_srgb_eotf(ss2h_clampf(G));
    out[2u * plane + idx] = vmaf_ss2_srgb_eotf(ss2h_clampf(B));
}

/* One pixel of ssimulacra2.c::linear_rgb_to_xyb (MakePositiveXYB folded in). */
__device__ inline void ss2h_xyb_pixel(const float *lin, float *xyb, size_t i, size_t plane)
{
    constexpr float kM00 = 0.30f;
    constexpr float kM02 = 0.078f;
    constexpr float kM10 = 0.23f;
    constexpr float kM12 = 0.078f;
    constexpr float kM20 = 0.24342268924547819f;
    constexpr float kM21 = 0.20476744424496821f;
    constexpr float kOpsinBias = 0.0037930732552754493f;
    constexpr float m01 = 1.0f - kM00 - kM02;
    constexpr float m11 = 1.0f - kM10 - kM12;
    constexpr float m22 = 1.0f - kM20 - kM21;
    const float cbrt_bias = vmaf_ss2_cbrtf(kOpsinBias);

    const float r = lin[i];
    const float g = lin[plane + i];
    const float b = lin[2u * plane + i];
    const float l_r = kM00 * r;
    const float l_g = m01 * g;
    const float l_b = kM02 * b;
    const float m_r = kM10 * r;
    const float m_g = m11 * g;
    const float m_b = kM12 * b;
    const float s_r = kM20 * r;
    const float s_g = kM21 * g;
    const float s_b = m22 * b;
    const float l = ((l_r + l_g) + l_b) + kOpsinBias;
    const float m = ((m_r + m_g) + m_b) + kOpsinBias;
    const float s = ((s_r + s_g) + s_b) + kOpsinBias;
    /* `if (l < 0.0f) l = 0.0f;` of the reference, NaN kept (not fmax). */
    const float L = vmaf_ss2_cbrtf((l < 0.0f) ? 0.0f : l) - cbrt_bias;
    const float M = vmaf_ss2_cbrtf((m < 0.0f) ? 0.0f : m) - cbrt_bias;
    const float S = vmaf_ss2_cbrtf((s < 0.0f) ? 0.0f : s) - cbrt_bias;
    const float X = 0.5f * (L - M);
    const float Yv = 0.5f * (L + M);
    const float X14 = X * 14.0f;
    xyb[i] = X14 + 0.42f;
    xyb[plane + i] = Yv + 0.01f;
    xyb[2u * plane + i] = (S - Yv) + 0.55f; /* B uses Y before its offset */
}

/* One output pixel of ssimulacra2.c::downsample_2x2, all three planes. */
__device__ inline void ss2h_down_pixel(const float *in, float *out, const Ss2hPlanesArgs &a,
                                       unsigned ox, unsigned oy)
{
    const size_t in_plane = (size_t)a.width * a.height;
    const size_t out_plane = (size_t)a.out_width * a.out_height;
    for (unsigned c = 0; c < SS2H_CHANNELS; c++) {
        const float *ip = in + c * in_plane;
        float sum = 0.0f;
        for (unsigned dy = 0; dy < 2u; dy++) {
            for (unsigned dx = 0; dx < 2u; dx++) {
                const unsigned ix = min(ox * 2u + dx, a.width - 1u);
                const unsigned iy = min(oy * 2u + dy, a.height - 1u);
                sum += ip[(size_t)iy * a.width + ix];
            }
        }
        out[c * out_plane + (size_t)oy * a.out_width + ox] = sum * 0.25f;
    }
}

/* ------------------------------------------------------------------ */
/* Separable recursive Gaussian                                        */
/* ------------------------------------------------------------------ */

struct Ss2hIirState {
    float prev1[3];
    float prev2[3];
};

/* One step of ssimulacra2.c::fast_gaussian_1d; returns o0 + o1 + o2. */
__device__ inline float ss2h_iir_step(float sum, Ss2hIirState &st, const Ss2hIir &k)
{
    float o[3];
#pragma unroll
    for (int p = 0; p < 3; p++) {
        const float ns = k.n2[p] * sum;
        const float dp = k.d1[p] * st.prev1[p];
        const float t = ns - dp;
        o[p] = t - st.prev2[p];
        st.prev2[p] = st.prev1[p];
        st.prev1[p] = o[p];
    }
    const float s01 = o[0] + o[1];
    return s01 + o[2];
}

/* ssimulacra2.c::fast_gaussian_1d along one row (step 1) or one column
 * (step = width) that starts at `base`. */
__device__ inline void ss2h_iir_line(const Ss2hBlurArgs &a, size_t base, size_t step, int xsize)
{
    const int radius = a.iir.radius;
    Ss2hIirState st = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    for (int n = -radius + 1; n < xsize; ++n) {
        const int left = n - radius - 1;
        const int right = n + radius - 1;
        const float lv = (left >= 0) ? a.in[base + (size_t)left * step] : 0.0f;
        const float rv = (right < xsize) ? a.in[base + (size_t)right * step] : 0.0f;
        const float s = ss2h_iir_step(lv + rv, st, a.iir);
        if (n >= 0) {
            a.out[base + (size_t)n * step] = s;
        }
    }
}

/* ------------------------------------------------------------------ */
/* SSIM and edge-difference terms, fixed-tree sums                     */
/* ------------------------------------------------------------------ */

struct Ss2hSums {
    Ff v[SS2H_SUMS];
};

/* ssimulacra2.c::ssim_map per pixel: the fp32 part verbatim, then
 * d = 1.0 - (double)num_m * (double)num_s / (double)denom_s in fp32 pairs
 * (the product of two floats is exact as a pair), clamped at zero. */
__device__ inline Ff ss2h_ssim_term(float u1, float u2, float s11, float s22, float s12)
{
    const float u11 = u1 * u1;
    const float u22 = u2 * u2;
    const float u12 = u1 * u2;
    const float diff = u1 - u2;
    const float diff_sq = diff * diff;
    const float num_m = 1.0f - diff_sq;
    const float cov = s12 - u12;
    const float cov2 = 2.0f * cov;
    const float num_s = cov2 + kSs2C2;
    const float var1 = s11 - u11;
    const float var2 = s22 - u22;
    const float var_sum = var1 + var2;
    const float denom_s = var_sum + kSs2C2;
    const Ff ratio = ff_div(two_prod(num_m, num_s), Ff{denom_s, 0.0f});
    const Ff d = ff_add(kFfOne, ff_neg(ratio));
    return (d.hi < 0.0f) ? kFfZero : d; /* NaN is kept, as on the host */
}

/* |(double)a - (double)b| exactly. */
__device__ inline Ff ss2h_abs_diff(float a, float b)
{
    const Ff diff = two_sum(a, -b);
    return (diff.hi < 0.0f) ? ff_neg(diff) : diff;
}

/* ssimulacra2.c::edge_diff_map per pixel: (1 + ed2) / (1 + ed1) - 1 and
 * vmaf_ss2_split_edge_difference (a non-finite value goes to both sums). */
__device__ inline void ss2h_edge_terms(float r1, float m1, float r2, float m2, Ff &artifact,
                                       Ff &detail)
{
    const Ff num = ff_add(kFfOne, ss2h_abs_diff(r2, m2));
    const Ff den = ff_add(kFfOne, ss2h_abs_diff(r1, m1));
    const Ff dd = ff_add(ff_div(num, den), kFfMinusOne);
    if (!isfinite(dd.hi)) {
        artifact = dd;
        detail = dd;
        return;
    }
    artifact = (dd.hi > 0.0f) ? dd : kFfZero;
    detail = (dd.hi < 0.0f) ? ff_neg(dd) : kFfZero;
}

__device__ inline Ff ss2h_quartic(Ff x)
{
    const Ff sq = ff_mul(x, x);
    return ff_mul(sq, sq);
}

__device__ inline void ss2h_accumulate_pixel(Ss2hSums &acc, const Ss2hCombineArgs &a, size_t idx)
{
    const float u1 = a.mu1[idx];
    const float u2 = a.mu2[idx];
    const Ff d = ss2h_ssim_term(u1, u2, a.s11[idx], a.s22[idx], a.s12[idx]);
    Ff artifact = kFfZero;
    Ff detail = kFfZero;
    ss2h_edge_terms(a.img1[idx], u1, a.img2[idx], u2, artifact, detail);
    acc.v[0] = ff_add(acc.v[0], d);
    acc.v[1] = ff_add(acc.v[1], ss2h_quartic(d));
    acc.v[2] = ff_add(acc.v[2], artifact);
    acc.v[3] = ff_add(acc.v[3], ss2h_quartic(artifact));
    acc.v[4] = ff_add(acc.v[4], detail);
    acc.v[5] = ff_add(acc.v[5], ss2h_quartic(detail));
}

/* Fixed-shape tree over one work-group's sums in LDS; lane 0 ends up with the
 * group total. A shared-memory tree over every lane, not wavefront shuffles,
 * so wave32 and wave64 devices add in the same order. */
__device__ inline void ss2h_group_tree(float *lds, unsigned lane, const Ss2hSums &own)
{
    for (unsigned s = 0; s < SS2H_SUMS; s++) {
        const size_t at = (size_t)lane * SS2H_LANE_FLOATS + (size_t)s * SS2H_PAIR;
        lds[at] = own.v[s].hi;
        lds[at + 1u] = own.v[s].lo;
    }
    __syncthreads();
    for (unsigned stride = SS2H_REDUCE_WG / 2u; stride > 0u; stride >>= 1u) {
        if (lane < stride) {
            float *mine = lds + (size_t)lane * SS2H_LANE_FLOATS;
            const float *other = lds + (size_t)(lane + stride) * SS2H_LANE_FLOATS;
            for (unsigned s = 0; s < SS2H_SUMS; s++) {
                const size_t at = (size_t)s * SS2H_PAIR;
                const Ff merged =
                    ff_add(Ff{mine[at], mine[at + 1u]}, Ff{other[at], other[at + 1u]});
                mine[at] = merged.hi;
                mine[at + 1u] = merged.lo;
            }
        }
        __syncthreads();
    }
}

/* ------------------------------------------------------------------ */
/* Row pass through LDS                                                */
/* ------------------------------------------------------------------ */

/* Walking a row in global memory, one lane per row, makes every load of a
 * wavefront touch as many cache lines as it has lanes; on the gfx1036 that
 * pass took 42x the time of the lane-per-column pass (322 vs 7.7 ms per 4K
 * launch, ADR-1390). The row pass instead runs one wavefront per
 * SS2H_ROW_TILE rows: it loads SS2H_ROW_TILE-column tiles of its rows with
 * consecutive lanes on consecutive columns, each lane walks its own row
 * through the tile, and the lanes store the tile's outputs the same way. */

/* One plane of a row pass: its rows row0 .. row0 + SS2H_ROW_TILE - 1. */
struct Ss2hRowPlane {
    const float *a;
    const float *b; /* second factor of a product, or nullptr */
    float *out;
    unsigned row0;
};

__device__ inline Ss2hRowPlane ss2h_row_plane(const Ss2hBlurArgs &args, unsigned c, unsigned row0)
{
    const size_t plane = (size_t)c * args.width * args.height;
    Ss2hRowPlane rows;
    rows.a = args.in + plane;
    rows.b = (args.in2 != nullptr) ? args.in2 + plane : nullptr;
    rows.out = args.out + plane;
    rows.row0 = row0;
    return rows;
}

/* Tile geometry of the row pass: SS2H_ROW_TILE rows (one per lane) by
 * SS2H_ROW_COLS columns. A load instruction covers kRowsPerLoad rows of
 * SS2H_ROW_COLS consecutive columns, so every lane issues kTileLoads loads
 * per tile (twice that for a product), within the 63 loads a gfx10 wavefront
 * can keep in flight. */
constexpr unsigned kRowsPerLoad = SS2H_ROW_TILE / SS2H_ROW_COLS;
constexpr unsigned kTileLoads = SS2H_ROW_TILE / kRowsPerLoad;

/* This lane's share of tile `chunk` (columns chunk * SS2H_ROW_COLS ..) into
 * registers, as fast_gaussian_1d reads it: zero right of the image (and on
 * rows past its end), the product formed here for kProduct (the single fp32
 * multiply of ssimulacra2.c::multiply_3plane). Every lane loads
 * unconditionally, out-of-range lanes from element 0, so the loads of a tile
 * issue back to back instead of one branch and one wait per row. */
template <bool kProduct>
__device__ inline void ss2h_row_tile_load(const Ss2hRowPlane &rows, const Ss2hBlurArgs &args,
                                          int chunk, unsigned lane, float vals[kTileLoads])
{
    const unsigned col = ((unsigned)chunk * SS2H_ROW_COLS) + (lane % SS2H_ROW_COLS);
    const unsigned sub = lane / SS2H_ROW_COLS;
    const bool col_ok = col < args.width;
#pragma unroll
    for (unsigned j = 0; j < kTileLoads; j++) {
        const unsigned row = rows.row0 + (j * kRowsPerLoad) + sub;
        const bool inside = col_ok && row < args.height;
        const size_t idx = inside ? ((size_t)row * args.width) + col : 0u;
        float v = rows.a[idx];
        if constexpr (kProduct) {
            const float pb = rows.b[idx];
            v = v * pb;
        }
        vals[j] = inside ? v : 0.0f;
    }
}

/* ssimulacra2.c::fast_gaussian_1d along SS2H_ROW_TILE rows of one plane, one
 * lane per row (see ssimulacra2_blur_rows). */
template <bool kProduct>
__device__ inline void ss2h_blur_rows(const Ss2hBlurArgs &args,
                                      float (*tile)[SS2H_ROW_TILE][SS2H_ROW_COLS + 1],
                                      float (*outs)[SS2H_ROW_COLS + 1])
{
    constexpr int kCols = (int)SS2H_ROW_COLS;
    const unsigned lane = threadIdx.x;
    const unsigned my_col = lane % SS2H_ROW_COLS;
    const unsigned sub = lane / SS2H_ROW_COLS;
    const Ss2hRowPlane rows = ss2h_row_plane(args, blockIdx.y, blockIdx.x * SS2H_ROW_TILE);
    const int width = (int)args.width;
    const int radius = args.iir.radius;
    const int lead = radius - 1;

    float vals[kTileLoads];
    ss2h_row_tile_load<kProduct>(rows, args, 0, lane, vals);
#pragma unroll
    for (unsigned j = 0; j < kTileLoads; j++) {
        tile[0][(j * kRowsPerLoad) + sub][my_col] = vals[j];
        tile[1][(j * kRowsPerLoad) + sub][my_col] = 0.0f; /* tile -1: left of column 0 */
    }
    __syncwarp();

    Ss2hIirState st = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    for (int chunk = 0; (chunk * kCols) - lead < width; chunk++) {
        const int first = (chunk * kCols) - lead;
        ss2h_row_tile_load<kProduct>(rows, args, chunk + 1, lane, vals);
        const float *right = tile[chunk & 1][lane];
        for (int k = 0; k < kCols && first + k < width; k++) {
            const int n = first + k;
            const int left = n - radius - 1;
            const float lv = (left >= 0) ? tile[(left / kCols) & 1][lane][left % kCols] : 0.0f;
            const float s = ss2h_iir_step(lv + right[k], st, args.iir);
            if (n >= 0) {
                outs[lane][k] = s;
            }
        }
        __syncwarp();
        const int col = first + (int)my_col;
        if (col >= 0 && col < width) {
#pragma unroll
            for (unsigned j = 0; j < kTileLoads; j++) {
                const unsigned r = (j * kRowsPerLoad) + sub;
                if (rows.row0 + r < args.height) {
                    rows.out[((size_t)(rows.row0 + r) * args.width) + (size_t)col] =
                        outs[r][my_col];
                }
            }
        }
#pragma unroll
        for (unsigned j = 0; j < kTileLoads; j++) {
            tile[(chunk + 1) & 1][(j * kRowsPerLoad) + sub][my_col] = vals[j];
        }
        __syncwarp();
    }
}

} // namespace

extern "C" {

/* 2D grid of SS2H_PIX_BX x SS2H_PIX_BY blocks over the full frame. */
__global__
__launch_bounds__(SS2H_PIX_BX *SS2H_PIX_BY) void ssimulacra2_yuv_to_linear(Ss2hYuvArgs args)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= args.width || y >= args.height) {
        return;
    }
    for (unsigned img = 0; img < SS2H_IMAGES; img++) {
        ss2h_yuv_pixel(args, img, x, y);
    }
}

/* 1D, one work-item per pixel of the current scale. */
__global__ __launch_bounds__(SS2H_ELEM_BLOCK) void ssimulacra2_xyb(Ss2hPlanesArgs args)
{
    const size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    const size_t plane = (size_t)args.width * args.height;
    if (i >= plane) {
        return;
    }
    for (unsigned img = 0; img < SS2H_IMAGES; img++) {
        ss2h_xyb_pixel(args.in[img], args.out[img], i, plane);
    }
}

/* One wavefront per SS2H_ROW_TILE rows of one plane (grid: row tiles x 3
 * planes, SS2H_ROW_TILE lanes), one lane per row: ssimulacra2.c::
 * fast_gaussian_1d along every row. Chunk c walks the steps
 * n = c * SS2H_ROW_COLS - (radius - 1) + k, k < SS2H_ROW_COLS. Their right
 * input n + radius - 1 is column k of tile c; their left input
 * n - radius - 1 lies in tile c - 1 or c because the host keeps
 * 2 * radius <= SS2H_ROW_COLS, so two tile slots suffice. Tile c + 1 is
 * loaded into registers while tile c is walked. ssimulacra2_blur_rows blurs
 * `in`, ssimulacra2_blur_rows_product the product in * in2. */
__global__ __launch_bounds__(SS2H_ROW_TILE) void ssimulacra2_blur_rows(Ss2hBlurArgs args)
{
    __shared__ float tile[2][SS2H_ROW_TILE][SS2H_ROW_COLS + 1];
    __shared__ float outs[SS2H_ROW_TILE][SS2H_ROW_COLS + 1];
    ss2h_blur_rows<false>(args, tile, outs);
}

__global__ __launch_bounds__(SS2H_ROW_TILE) void ssimulacra2_blur_rows_product(Ss2hBlurArgs args)
{
    __shared__ float tile[2][SS2H_ROW_TILE][SS2H_ROW_COLS + 1];
    __shared__ float outs[SS2H_ROW_TILE][SS2H_ROW_COLS + 1];
    ss2h_blur_rows<true>(args, tile, outs);
}

/* One work-item per column of the three planes (3 * width lines). */
__global__ __launch_bounds__(SS2H_BLUR_BLOCK) void ssimulacra2_blur_cols(Ss2hBlurArgs args)
{
    const unsigned line = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned lines = args.width * SS2H_CHANNELS;
    if (line >= lines) {
        return;
    }
    const unsigned c = line / args.width;
    const unsigned l = line - c * args.width;
    const size_t base = (size_t)c * args.width * args.height + (size_t)l;
    ss2h_iir_line(args, base, (size_t)args.width, (int)args.height);
}

/* Stage 1 of the sums: grid (groups, 3); every work-group sums a fixed
 * strided subset of one channel and writes its six pairs to `partials`. */
__global__ __launch_bounds__(SS2H_REDUCE_WG) void ssimulacra2_combine_partials(Ss2hCombineArgs args)
{
    __shared__ float lds[SS2H_REDUCE_WG * SS2H_LANE_FLOATS];
    const unsigned group = blockIdx.x;
    const unsigned c = blockIdx.y;
    const unsigned lane = threadIdx.x;
    const size_t stride = (size_t)args.groups * SS2H_REDUCE_WG;
    const size_t offset = (size_t)c * args.plane;
    Ss2hSums acc = {};
    for (size_t i = (size_t)group * SS2H_REDUCE_WG + lane; i < args.plane; i += stride) {
        ss2h_accumulate_pixel(acc, args, offset + i);
    }
    ss2h_group_tree(lds, lane, acc);
    if (lane == 0u) {
        float *dst = args.partials + ((size_t)c * SS2H_MAX_GROUPS + group) * SS2H_LANE_FLOATS;
        for (unsigned f = 0; f < SS2H_LANE_FLOATS; f++) {
            dst[f] = lds[f];
        }
    }
}

/* Stage 2: one work-group per channel folds that channel's group partials,
 * each lane a fixed subset, then the LDS tree; lane 0 writes the six sums. */
__global__ __launch_bounds__(SS2H_REDUCE_WG) void ssimulacra2_combine_final(Ss2hFinalArgs args)
{
    __shared__ float lds[SS2H_REDUCE_WG * SS2H_LANE_FLOATS];
    const unsigned c = blockIdx.x;
    const unsigned lane = threadIdx.x;
    Ss2hSums acc = {};
    for (unsigned g = lane; g < args.groups; g += SS2H_REDUCE_WG) {
        const float *src = args.partials + ((size_t)c * SS2H_MAX_GROUPS + g) * SS2H_LANE_FLOATS;
        for (unsigned s = 0; s < SS2H_SUMS; s++) {
            const size_t at = (size_t)s * SS2H_PAIR;
            acc.v[s] = ff_add(acc.v[s], Ff{src[at], src[at + 1u]});
        }
    }
    ss2h_group_tree(lds, lane, acc);
    if (lane == 0u) {
        float *dst = args.totals + (size_t)c * SS2H_LANE_FLOATS;
        for (unsigned f = 0; f < SS2H_LANE_FLOATS; f++) {
            dst[f] = lds[f];
        }
    }
}

/* 2D grid over the next scale; both images, three planes each. */
__global__
__launch_bounds__(SS2H_PIX_BX *SS2H_PIX_BY) void ssimulacra2_downsample(Ss2hPlanesArgs args)
{
    const unsigned ox = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned oy = blockIdx.y * blockDim.y + threadIdx.y;
    if (ox >= args.out_width || oy >= args.out_height) {
        return;
    }
    for (unsigned img = 0; img < SS2H_IMAGES; img++) {
        ss2h_down_pixel(args.in[img], args.out[img], args, ox, oy);
    }
}

} /* extern "C" */
