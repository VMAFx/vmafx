/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Device-resident SpEED chain shared by speed_chroma_cuda and
 *  speed_temporal_cuda (ADR-1380, the CUDA port of ADR-1358's SYCL pipeline).
 *
 *  Every per-frame stage of the CPU reference (speed.c) up to the solved
 *  linear system runs here, in order, on the extractor's stream: optional
 *  prescale, the anti-alias filter at the 16x-decimated sample points
 *  (picture conversion and temporal difference folded into the sample read),
 *  local mean subtraction and the independent term, the 25 submatrix means,
 *  the 25x25 covariance matrix, Householder tridiagonalisation with the
 *  implicit-shift QR sweep, the regularity decision, the Householder QR
 *  factorisation, the Q^T B multiply with back substitution, and the
 *  variances. The host reads one block back per frame (the status words, the
 *  eigenvalues and the variances, SpeedGpuTailLayout) and forms the entropies
 *  and the score from it with speed.c's own statements and the host's log2()
 *  (speed_internal_gpu_tail_scores(), ADR-1477). No kernel evaluates a
 *  logarithm.
 *
 *  Numerical contract: every device routine is a line-for-line port of its CPU
 *  reference in speed.c / vif_tools.c / convolution_internal.h, and of the
 *  SYCL port in core/src/feature/sycl/speed_sycl_pipeline.cpp, which names the
 *  same references. Every fp32 operation whose rounding must match the host is
 *  written as an explicit round-to-nearest intrinsic (rn_add / rn_sub /
 *  rn_mul / rn_div / rn_sqrt below: __fadd_rn, __fsub_rn, __fmul_rn,
 *  __fdiv_rn, __fsqrt_rn). nvcc never fuses those into an FMA, so the kernels
 *  round exactly where the host rounds whatever the TU's --fmad setting; the
 *  fatbin is additionally built with --fmad=false (core/src/meson.build).
 *  __fdiv_rn and __fsqrt_rn are correctly rounded, so the SYCL port's
 *  residual refinement of the hardware division and square root is not needed
 *  here. The one explicit FMA, exact_fma(), is an error-free transform: it
 *  produces the exact low part of a product or the exact residual of a
 *  quotient, never a rounding the CPU performs.
 *
 *  The reference evaluates a few expressions in fp64 (the covariance sum, the
 *  EIGENVALUE_EPS comparisons, the prescale sample coordinates, the Givens
 *  rotation's `1.0 / sqrt(1 + t * t)`); they are reproduced with exact fp32
 *  pairs or, for the rotation, with feature/speed_givens.h, because no device
 *  kernel may use fp64.
 *  This file must not mention the fp64 type at all:
 *  core/test/test_cuda_device_resident_contract.py enforces that.
 */

#include <cstddef>
#include <cstdint>

#include "cuda_device_ptr.cuh"
#include "feature/cuda/speed/speed_cuda_params.h"

namespace
{

#define SPEED_KN 25u         /* elements_in_block */
#define SPEED_KMATRIX 625u   /* 25 x 25 */
#define SPEED_KTRIANGLE 325u /* lower triangle incl. diagonal */

constexpr uint32_t kN = SPEED_KN;
constexpr uint32_t kMatrix = SPEED_KMATRIX;
constexpr uint32_t kTriangle = SPEED_KTRIANGLE;
constexpr uint32_t kBlock = SPEED_GPU_BLOCK;
constexpr uint32_t kMaxTaps = SPEED_GPU_MAX_TAPS;
constexpr uint32_t kLanczosTaps = SPEED_GPU_LANCZOS_TAPS;
constexpr uint32_t kDecimation = 16u;      /* 2^NUM_SCALES */
constexpr uint32_t kQrIterationCap = 500u; /* EIGENVALUE_MAX_ITERS */
constexpr uint32_t kMeanChunk = 16u;
constexpr float kPictureOffset = -128.0f; /* picture_copy() offset */
constexpr float kElementsF = 25.0f;       /* elements_in_block */
constexpr float kEpsHi = 0x1.0c6f7ap-20f; /* (float)EIGENVALUE_EPS */
constexpr float kEpsLo = 0x1.6bdb1ap-49f; /* EIGENVALUE_EPS - kEpsHi, exact */

/* ------------------------------------------------------------------ */
/* Round-to-nearest fp32 arithmetic (never contracted)                 */
/* ------------------------------------------------------------------ */

__device__ __forceinline__ float rn_add(float a, float b)
{
    return __fadd_rn(a, b);
}

__device__ __forceinline__ float rn_sub(float a, float b)
{
    return __fsub_rn(a, b);
}

__device__ __forceinline__ float rn_mul(float a, float b)
{
    return __fmul_rn(a, b);
}

__device__ __forceinline__ float rn_div(float a, float b)
{
    return __fdiv_rn(a, b);
}

__device__ __forceinline__ float rn_sqrt(float x)
{
    return __fsqrt_rn(x);
}

/* Error-free transform only: `a * b + c` rounded once, used where the result
 * is exact (the low part of a product, a quotient's residual). */
__device__ __forceinline__ float exact_fma(float a, float b, float c)
{
    return __fmaf_rn(a, b, c);
}

/* create_givens()'s fp64 statement on the primitives above (ADR-1477). */
#define SPEED_GIVENS_FUNC __device__ __forceinline__
#define SPEED_GIVENS_SQRT(x) rn_sqrt(x)
#define SPEED_GIVENS_DIV(a, b) rn_div((a), (b))
#define SPEED_GIVENS_MUL(a, b) rn_mul((a), (b))
#define SPEED_GIVENS_SUB(a, b) rn_sub((a), (b))
#define SPEED_GIVENS_ADD(a, b) rn_add((a), (b))
#define SPEED_GIVENS_FMA(a, b, c) exact_fma((a), (b), (c))
#include "feature/speed_givens.h"

/* ------------------------------------------------------------------ */
/* Exact fp32 pair arithmetic                                          */
/* ------------------------------------------------------------------ */

struct Ff {
    float hi;
    float lo;
};

/* An aggregate is filled field by field: nvcc's MSVC host frontend rejects
 * designated initializers in device code (preflight --stage msvcism), and a
 * constructor would make the members private by clang-tidy's rules. */
__device__ __forceinline__ Ff make_ff(float hi, float lo)
{
    Ff pair;
    pair.hi = hi;
    pair.lo = lo;
    return pair;
}

__device__ __forceinline__ Ff two_sum(float a, float b)
{
    const float sum = rn_add(a, b);
    const float b_virtual = rn_sub(sum, a);
    const float a_virtual = rn_sub(sum, b_virtual);
    const float b_error = rn_sub(b, b_virtual);
    const float a_error = rn_sub(a, a_virtual);
    return make_ff(sum, rn_add(a_error, b_error));
}

__device__ __forceinline__ Ff quick_two_sum(float a, float b)
{
    const float sum = rn_add(a, b);
    const float rebuilt = rn_sub(sum, a);
    return make_ff(sum, rn_sub(b, rebuilt));
}

__device__ __forceinline__ Ff two_prod(float a, float b)
{
    const float product = rn_mul(a, b);
    return make_ff(product, exact_fma(a, b, -product));
}

__device__ __forceinline__ Ff ff_add(Ff a, Ff b)
{
    const Ff high = two_sum(a.hi, b.hi);
    const Ff low = two_sum(a.lo, b.lo);
    const Ff first = quick_two_sum(high.hi, rn_add(high.lo, low.hi));
    return quick_two_sum(first.hi, rn_add(low.lo, first.lo));
}

/* An element count as an exact fp32 pair: hi is the count rounded to fp32,
 * lo the rest. lo is 0 up to 2^24 and exact above it (below 2^48). */
__device__ __forceinline__ Ff count_ff(uint32_t count)
{
    const float hi = static_cast<float>(count);
    const float lo = static_cast<float>(static_cast<int64_t>(count) - static_cast<int64_t>(hi));
    return make_ff(hi, lo);
}

/* (hi + lo) / (divisor.hi + divisor.lo), rounded to fp32. speed.c divides its
 * accumulated sum by the exact size_t count; a count above 2^24 has no fp32 value,
 * so its rest enters the remainder. With divisor.lo == 0 this is the one-float
 * division bit for bit. */
__device__ __forceinline__ float ff_div_to_float(Ff value, Ff divisor)
{
    const float quotient = rn_div(value.hi, divisor.hi);
    float remainder = rn_add(exact_fma(-quotient, divisor.hi, value.hi), value.lo);
    if (divisor.lo != 0.0f) {
        remainder = rn_sub(remainder, rn_mul(quotient, divisor.lo));
    }
    const float correction = rn_div(remainder, divisor.hi);
    return rn_add(quotient, correction);
}

/* speed.c compares against EIGENVALUE_EPS = 1e-6, an fp64 constant, after
 * promoting the fp32 operands. `a < 1e-6 * s` is decided exactly here from
 * kEpsHi + kEpsLo == 1e-6 and two exact products. */
__device__ __forceinline__ bool below_eps_scaled(float a, float s)
{
    const Ff major = two_prod(kEpsHi, s);
    const Ff minor = two_prod(kEpsLo, s);
    const float tail = rn_add(rn_add(major.lo, minor.hi), minor.lo);
    if (a < rn_mul(0.5f, major.hi) || a > rn_mul(2.0f, major.hi))
        return a < major.hi;
    const float gap = rn_sub(a, major.hi); /* exact (Sterbenz) */
    return gap < tail;
}

/* `x < 1e-6` with fp64 promotion: no fp32 value lies strictly between kEpsHi
 * and 1e-6, so this is `x <= kEpsHi`. */
__device__ __forceinline__ bool below_eps(float x)
{
    return x <= kEpsHi;
}

/* ------------------------------------------------------------------ */
/* Picture sources                                                     */
/* ------------------------------------------------------------------ */

/* convolution_reflect101(), convolution_internal.h. */
__device__ __forceinline__ uint32_t reflect101(int32_t index, uint32_t size)
{
    const int32_t n = static_cast<int32_t>(size);
    if (n <= 1)
        return 0u;
    int32_t folded = index;
    for (uint32_t bounce = 0; bounce < 8u && (folded < 0 || folded >= n); bounce++)
        folded = folded < 0 ? -folded : 2 * n - folded - 2;
    return static_cast<uint32_t>(folded);
}

/* Whether every tap of a `2 * radius + 1` window around `centre` is inside
 * [0, size), so the reflect-101 fold can be skipped. */
__device__ __forceinline__ bool window_inside(int32_t centre, int32_t radius, uint32_t size)
{
    return centre >= radius && centre + radius < static_cast<int32_t>(size);
}

/* Plane index of tap `k` around `centre`. */
__device__ __forceinline__ uint32_t tap(int32_t centre, int32_t radius, uint32_t k, uint32_t size,
                                        bool inside)
{
    const int32_t index = centre - radius + static_cast<int32_t>(k);
    return inside ? static_cast<uint32_t>(index) : reflect101(index, size);
}

/* picture_copy(): 8-bit `(float)v + offset`, else `(float)v / scaler + offset`. */
template <typename T>
__device__ __forceinline__ float picture_value(const T *__restrict__ plane, size_t offset,
                                               float scale)
{
    const float sample = static_cast<float>(plane[offset]);
    if constexpr (sizeof(T) == 1u) {
        (void)scale;
        return rn_add(sample, kPictureOffset);
    } else {
        return rn_add(rn_div(sample, scale), kPictureOffset);
    }
}

/* Converted picture, or the temporal difference `minuend - subtrahend` of two
 * converted pictures (speed.c extract(): subtract_image()). */
template <typename T> class RawSource
{
  public:
    __device__ explicit RawSource(const SpeedCudaFrameArgs &a)
        : raw_(VMAF_CUDA_DPTR(const unsigned char, a.raw)), plane_bytes_(a.plane_bytes),
          bindings_(a.bindings), width_(a.geometry.src_w), scale_(a.geometry.sample_scale)
    {
    }

    __device__ float operator()(uint32_t channel, uint32_t row, uint32_t col) const
    {
        const size_t offset = static_cast<size_t>(row) * width_ + col;
        const SpeedGpuChannelBinding binding = bindings_.channel[channel];
        const float value = picture_value<T>(plane(binding.minuend), offset, scale_);
        if (binding.subtrahend < 0)
            return value;
        const float other = picture_value<T>(plane(binding.subtrahend), offset, scale_);
        return rn_sub(value, other);
    }

  private:
    __device__ const T *plane(int32_t index) const
    {
        return reinterpret_cast<const T *>(raw_ + static_cast<size_t>(index) * plane_bytes_);
    }

    const unsigned char *raw_;
    uint64_t plane_bytes_;
    SpeedCudaBindings bindings_;
    uint32_t width_;
    float scale_;
};

/* A materialised float plane per channel, row stride `width`. */
class FloatSource
{
  public:
    __device__ FloatSource(const float *plane, uint32_t width, uint32_t height)
        : plane_(plane), width_(width), height_(height)
    {
    }

    __device__ float operator()(uint32_t channel, uint32_t row, uint32_t col) const
    {
        return plane_[(static_cast<size_t>(channel) * height_ + row) * width_ + col];
    }

  private:
    const float *plane_;
    uint32_t width_;
    uint32_t height_;
};

/* ------------------------------------------------------------------ */
/* Prescale: vif_scale_frame_s(), vif_tools.c                          */
/* ------------------------------------------------------------------ */

/* `(y + 0.5) * ratio - 0.5`, evaluated by the reference in fp64 and rounded
 * once to fp32. Every intermediate below is exact. */
__device__ __forceinline__ float centre_coordinate(uint32_t index, float ratio)
{
    const float centre = rn_add(static_cast<float>(index), 0.5f);
    const Ff scaled = two_prod(centre, ratio);
    const Ff shifted = two_sum(scaled.hi, -0.5f);
    const float tail = rn_add(shifted.lo, scaled.lo);
    return rn_add(shifted.hi, tail);
}

/* mirror(), vif_tools.c (float arguments). */
__device__ __forceinline__ float mirror_coordinate(float i, float right)
{
    if (i < 0.0f)
        return -i;
    if (i > right)
        return rn_sub(rn_mul(2.0f, right), i);
    return i;
}

template <class Source>
__device__ float scale_bilinear(const Source &src, uint32_t ch, const SpeedGpuGeometry &g, float x,
                                float y)
{
    const float right = static_cast<float>(g.src_w - 1u);
    const float bottom = static_cast<float>(g.src_h - 1u);
    const auto x1 = static_cast<int32_t>(mirror_coordinate(floorf(x), right));
    const auto x2 = static_cast<int32_t>(mirror_coordinate(ceilf(x), right));
    const auto y1 = static_cast<int32_t>(mirror_coordinate(floorf(y), bottom));
    const auto y2 = static_cast<int32_t>(mirror_coordinate(ceilf(y), bottom));
    const float dx = rn_sub(x, static_cast<float>(x1));
    const float dy = rn_sub(y, static_cast<float>(y1));
    const float ix = rn_sub(1.0f, dx);
    const float iy = rn_sub(1.0f, dy);
    const float t11 = rn_mul(rn_mul(iy, ix), src(ch, static_cast<uint32_t>(y1), x1));
    const float t12 = rn_mul(rn_mul(iy, dx), src(ch, static_cast<uint32_t>(y1), x2));
    const float t21 = rn_mul(rn_mul(dy, ix), src(ch, static_cast<uint32_t>(y2), x1));
    const float t22 = rn_mul(rn_mul(dy, dx), src(ch, static_cast<uint32_t>(y2), x2));
    return rn_add(rn_add(rn_add(t11, t12), t21), t22);
}

/* bicubic_kernel(), vif_tools.c. */
__device__ __forceinline__ float bicubic_weight(float t)
{
    const float a = -0.75f;
    const float u = t < 0.0f ? -t : t;
    if (u < 1.0f) {
        const float c1 = rn_mul(rn_add(a, 2.0f), u);
        const float c2 = rn_sub(c1, rn_add(a, 3.0f));
        const float c3 = rn_mul(c2, u);
        const float c4 = rn_mul(c3, u);
        return rn_add(c4, 1.0f);
    }
    if (u < 2.0f) {
        const float c1 = rn_mul(rn_sub(u, 5.0f), u);
        const float c2 = rn_mul(rn_add(c1, 8.0f), u);
        return rn_mul(rn_sub(c2, 4.0f), a);
    }
    return 0.0f;
}

template <class Source>
__device__ float scale_bicubic(const Source &src, uint32_t ch, const SpeedGpuGeometry &g, float x,
                               float y)
{
    const auto x0 = static_cast<int32_t>(floorf(x));
    const auto y0 = static_cast<int32_t>(floorf(y));
    const float dx = rn_sub(x, static_cast<float>(x0));
    const float dy = rn_sub(y, static_cast<float>(y0));
    float wx[4];
    float wy[4];
    for (int32_t i = -1; i <= 2; i++) {
        wx[i + 1] = bicubic_weight(rn_sub(static_cast<float>(i), dx));
        wy[i + 1] = bicubic_weight(rn_sub(static_cast<float>(i), dy));
    }
    const float right = static_cast<float>(g.src_w - 1u);
    const float bottom = static_cast<float>(g.src_h - 1u);
    float value = 0.0f;
    for (int32_t j = -1; j <= 2; j++) {
        for (int32_t i = -1; i <= 2; i++) {
            const auto xi =
                static_cast<int32_t>(mirror_coordinate(static_cast<float>(x0 + i), right));
            const auto yj =
                static_cast<int32_t>(mirror_coordinate(static_cast<float>(y0 + j), bottom));
            const float weight = rn_mul(wx[i + 1], wy[j + 1]);
            const float sample = src(ch, static_cast<uint32_t>(yj), static_cast<uint32_t>(xi));
            value = rn_add(value, rn_mul(sample, weight));
        }
    }
    return value;
}

/* lanczos4_interpolation(), vif_tools.c. The reference evaluates
 * lanczos4_kernel() in fp64 with sin() and rounds each weight once; no fp32
 * device sine reproduces that rounding, and SpEED amplifies the last-bit
 * differences on smooth content. The weights depend only on the output column
 * and row, so the host evaluates them once per run with the CPU scaler's own
 * routine (speed_internal_gpu_lanczos_weights()) and this kernel reads them:
 * `wx` are the nine taps of this output column, `wy` of this output row. */
template <class Source>
__device__ float scale_lanczos(const Source &src, uint32_t ch, const SpeedGpuGeometry &g,
                               const float *__restrict__ wx, const float *__restrict__ wy, float x,
                               float y)
{
    const auto x0 = static_cast<int32_t>(floorf(x));
    const auto y0 = static_cast<int32_t>(floorf(y));
    const float right = static_cast<float>(g.src_w - 1u);
    const float bottom = static_cast<float>(g.src_h - 1u);
    float value = 0.0f;
    float weight_sum = 0.0f;
    for (int32_t iy = -4; iy <= 4; iy++) {
        for (int32_t ix = -4; ix <= 4; ix++) {
            const float weight = rn_mul(wx[ix + 4], wy[iy + 4]);
            weight_sum = rn_add(weight_sum, weight);
            const auto xi =
                static_cast<int32_t>(mirror_coordinate(static_cast<float>(x0 + ix), right));
            const auto yi =
                static_cast<int32_t>(mirror_coordinate(static_cast<float>(y0 + iy), bottom));
            const float sample = src(ch, static_cast<uint32_t>(yi), static_cast<uint32_t>(xi));
            value = rn_add(value, rn_mul(sample, weight));
        }
    }
    return rn_div(value, weight_sum);
}

/* `lanczos`: the host's weight table, SPEED_GPU_LANCZOS_TAPS per scaled
 * column and then per scaled row; read by the lanczos4 method only. */
template <class Source>
__device__ float scale_sample(const Source &src, const SpeedGpuGeometry &g,
                              const float *__restrict__ lanczos, uint32_t ch, uint32_t y,
                              uint32_t x)
{
    if (g.src_w == g.scaled_w && g.src_h == g.scaled_h)
        return src(ch, y, x);
    const float ratio_x = rn_div(static_cast<float>(g.src_w), static_cast<float>(g.scaled_w));
    const float ratio_y = rn_div(static_cast<float>(g.src_h), static_cast<float>(g.scaled_h));
    if (g.scale_method == 0) { /* vif_scale_nearest */
        const auto sy = static_cast<uint32_t>(rn_mul(static_cast<float>(y), ratio_y));
        const auto sx = static_cast<uint32_t>(rn_mul(static_cast<float>(x), ratio_x));
        return src(ch, sy, sx);
    }
    const float xx = centre_coordinate(x, ratio_x);
    const float yy = centre_coordinate(y, ratio_y);
    if (g.scale_method == 1) /* vif_scale_bicubic */
        return scale_bicubic(src, ch, g, xx, yy);
    if (g.scale_method == 2) { /* vif_scale_lanczos4 */
        const float *wx = lanczos + static_cast<size_t>(kLanczosTaps) * x;
        const float *wy = lanczos + static_cast<size_t>(kLanczosTaps) * (g.scaled_w + y);
        return scale_lanczos(src, ch, g, wx, wy, xx, yy);
    }
    return scale_bilinear(src, ch, g, xx, yy); /* vif_scale_bilinear */
}

/* ------------------------------------------------------------------ */
/* Anti-alias filter at the decimated sample points                    */
/* vif_filter1d_dec16_s() on every target (Netflix/vmaf ad42c532)       */
/* ------------------------------------------------------------------ */

template <class Source>
__device__ float antialias_at(const Source &src, const float *__restrict__ taps, uint32_t width,
                              uint32_t src_w, uint32_t src_h, uint32_t ch, uint32_t i, uint32_t j)
{
    const auto radius = static_cast<int32_t>(width / 2u);
    const auto row = static_cast<int32_t>(i * kDecimation);
    const auto col = static_cast<int32_t>(j * kDecimation);
    const bool rows_inside = window_inside(row, radius, src_h);
    const bool cols_inside = window_inside(col, radius, src_w);
    float horizontal = 0.0f;
    for (uint32_t kx = 0; kx < width; kx++) {
        const uint32_t c = tap(col, radius, kx, src_w, cols_inside);
        float vertical = 0.0f;
        for (uint32_t ky = 0; ky < width; ky++) {
            const uint32_t r = tap(row, radius, ky, src_h, rows_inside);
            vertical = rn_add(vertical, rn_mul(taps[ky], src(ch, r, c)));
        }
        horizontal = rn_add(horizontal, rn_mul(taps[kx], vertical));
    }
    return horizontal;
}

/* ------------------------------------------------------------------ */
/* Local mean subtraction + independent term                           */
/* filter_and_downscale() tail + compute_independent_term()            */
/* ------------------------------------------------------------------ */

__device__ float lowpass_at(const float *__restrict__ plane, const float *__restrict__ taps,
                            uint32_t width, const SpeedGpuGeometry &g, uint32_t i, uint32_t j)
{
    const auto radius = static_cast<int32_t>(width / 2u);
    const auto row = static_cast<int32_t>(i);
    const auto col = static_cast<int32_t>(j);
    const bool rows_inside = window_inside(row, radius, g.down_h);
    const bool cols_inside = window_inside(col, radius, g.down_w);
    float horizontal = 0.0f;
    for (uint32_t kx = 0; kx < width; kx++) {
        const uint32_t c = tap(col, radius, kx, g.down_w, cols_inside);
        float vertical = 0.0f;
        for (uint32_t ky = 0; ky < width; ky++) {
            const uint32_t r = tap(row, radius, ky, g.down_h, rows_inside);
            const float sample = plane[static_cast<size_t>(r) * g.down_w + c];
            vertical = rn_add(vertical, rn_mul(taps[ky], sample));
        }
        horizontal = rn_add(horizontal, rn_mul(taps[kx], vertical));
    }
    return horizontal;
}

/* ------------------------------------------------------------------ */
/* Means: compute_mean(), one sequential fp32 sum per element          */
/* ------------------------------------------------------------------ */

/* One row of the sequential sum. The chunk's loads are independent of the
 * running sum, so issuing them together hides the memory latency; the adds
 * still happen one at a time in the reference order. */
__device__ __forceinline__ float add_row(float sum, const float *__restrict__ row, uint32_t width)
{
    uint32_t j = 0;
    for (; j + kMeanChunk <= width; j += kMeanChunk) {
        float chunk[kMeanChunk];
#pragma unroll
        for (uint32_t k = 0; k < kMeanChunk; k++)
            chunk[k] = row[j + k];
#pragma unroll
        for (const float value : chunk)
            sum = rn_add(sum, value);
    }
    for (; j < width; j++)
        sum = rn_add(sum, row[j]);
    return sum;
}

/* ------------------------------------------------------------------ */
/* Covariance: compute_covariance_matrix()                             */
/* The reference accumulates (x - mean_x) * (y - mean_y) in fp64. Here */
/* each difference and product is exact as an fp32 pair and the sum is */
/* carried in fp32 pairs; the quotient is rounded once to fp32.        */
/* ------------------------------------------------------------------ */

__device__ __forceinline__ void triangle_entry(uint32_t index, uint32_t &x, uint32_t &y)
{
    uint32_t rest = index;
    x = 0u;
    for (uint32_t row = 0; row < kN; row++) {
        if (rest <= row) {
            x = row;
            break;
        }
        rest -= row + 1u;
    }
    y = rest;
}

__device__ __forceinline__ Ff centred_product(float vx, float mx, float vy, float my)
{
    const Ff dx = two_sum(vx, -mx);
    const Ff dy = two_sum(vy, -my);
    const Ff main = two_prod(dx.hi, dy.hi);
    const float cross1 = rn_mul(dx.hi, dy.lo);
    const float cross2 = rn_mul(dx.lo, dy.hi);
    const float cross = rn_add(cross1, cross2);
    return make_ff(main.hi, rn_add(main.lo, cross));
}

/* Compensated accumulation of one term: the high parts are summed exactly,
 * every low-order part lands in `lo`; normalised once per thread. */
__device__ __forceinline__ Ff accumulate(Ff acc, Ff term)
{
    const Ff high = two_sum(acc.hi, term.hi);
    const float low = rn_add(term.lo, high.lo);
    return make_ff(high.hi, rn_add(acc.lo, low));
}

__device__ Ff covariance_partial(const float *__restrict__ plane, const SpeedGpuGeometry &g,
                                 uint32_t x, uint32_t y, float mx, float my, uint32_t lid,
                                 uint32_t threads)
{
    const uint32_t xr = x / kBlock;
    const uint32_t xc = x % kBlock;
    const uint32_t yr = y / kBlock;
    const uint32_t yc = y % kBlock;
    const uint32_t total = g.sub_w * g.sub_h;
    Ff acc = make_ff(0.0f, 0.0f);
    for (uint32_t pos = lid; pos < total; pos += threads) {
        const uint32_t row = pos / g.sub_w;
        const uint32_t col = pos % g.sub_w;
        const float vx = plane[static_cast<size_t>(xr + row) * g.trunc_w + xc + col];
        const float vy = plane[static_cast<size_t>(yr + row) * g.trunc_w + yc + col];
        acc = accumulate(acc, centred_product(vx, mx, vy, my));
    }
    return quick_two_sum(acc.hi, acc.lo);
}

/* ------------------------------------------------------------------ */
/* Eigenvalues: compute_eigenvalues() (Householder tridiagonalisation  */
/* + implicit-shift QR), regularity, matrix_qr_decomposition()         */
/* ------------------------------------------------------------------ */

constexpr size_t kSlmStride = 640u;
constexpr size_t kSlmVector = 32u;
constexpr size_t kSlmFloats = 6u * kSlmStride + 5u * kSlmVector;

struct Slm {
    float *a;   /* tridiagonalisation work */
    float *cov; /* covariance, untouched */
    float *z;   /* QR working matrix */
    float *q;   /* accumulated reflectors */
    float *h;   /* current reflector */
    float *t;   /* product scratch */
    float *vec;
    float *x;
    float *d;
    float *sd;
    float *scalar; /* [0] tau, [1] alpha, [2] norm, [3] regular, [4] capped, [5] s, [6] beta */
};

__device__ __forceinline__ Slm slm_layout(float *base)
{
    float *vectors = base + 6u * kSlmStride;
    Slm m;
    m.a = base;
    m.cov = base + kSlmStride;
    m.z = base + 2u * kSlmStride;
    m.q = base + 3u * kSlmStride;
    m.h = base + 4u * kSlmStride;
    m.t = base + 5u * kSlmStride;
    m.vec = vectors;
    m.x = vectors + kSlmVector;
    m.d = vectors + 2u * kSlmVector;
    m.sd = vectors + 3u * kSlmVector;
    m.scalar = vectors + 4u * kSlmVector;
    return m;
}

__device__ __forceinline__ float sign_of(float x)
{
    return x >= 0.0f ? 1.0f : -1.0f;
}

/* pythagoras(), speed.c. */
__device__ __forceinline__ float pythagoras(float x, float y)
{
    return rn_sqrt(rn_add(rn_mul(x, x), rn_mul(y, y)));
}

/* compute_column_norm(), speed.c. */
__device__ float column_norm(const float *a, uint32_t col, uint32_t start)
{
    float norm = 0.0f;
    for (uint32_t r = start; r < kN; r++) {
        const float value = a[r * kN + col];
        norm = rn_add(norm, rn_mul(value, value));
    }
    return rn_sqrt(norm);
}

/* The scalar half of compute_householder_transform(), speed.c, on one thread:
 * stores tau, the column divisor s (0 when the column is left alone) and
 * beta. The column division itself is element-wise, so the block does it
 * (householder_scale). */
__device__ void householder_scalars(const Slm &m, uint32_t col, uint32_t start)
{
    m.scalar[0] = 0.0f;
    m.scalar[5] = 0.0f;
    if (kN - start == 1u)
        return;
    const float xnorm = column_norm(m.a, col, start + 1u);
    if (xnorm == 0.0f)
        return;
    const float alpha = m.a[start * kN + col];
    const float beta = rn_mul(-sign_of(alpha), pythagoras(alpha, xnorm));
    m.scalar[0] = rn_div(rn_sub(beta, alpha), beta);
    m.scalar[5] = rn_sub(alpha, beta);
    m.scalar[6] = beta;
}

/* `A[i][col] /= s` for the column, then `A[start][col] = beta`. `s` is read
 * after a barrier, so every thread takes the same branch. */
__device__ void householder_scale(const Slm &m, uint32_t col, uint32_t start)
{
    const uint32_t lid = threadIdx.x;
    const float s = m.scalar[5];
    if (s == 0.0f)
        return;
    for (uint32_t r = start + lid; r < kN; r += blockDim.x)
        m.a[r * kN + col] = rn_div(m.a[r * kN + col], s);
    __syncthreads();
    if (lid == 0u)
        m.a[start * kN + col] = m.scalar[6];
    __syncthreads();
}

/* x = tau * A * v, then x += alpha * v (tridiagonal_multiply/_axpy). */
__device__ void tridiagonal_vectors(const Slm &m, uint32_t start, float tau)
{
    const uint32_t lid = threadIdx.x;
    for (uint32_t r = start + lid; r < kN; r += blockDim.x) {
        float acc = 0.0f;
        for (uint32_t c = start; c < kN; c++) {
            const float scaled = rn_mul(tau, m.a[r * kN + c]);
            acc = rn_add(acc, rn_mul(scaled, m.vec[c]));
        }
        m.x[r] = acc;
    }
    __syncthreads();
    if (lid == 0u) {
        float xv = 0.0f;
        for (uint32_t r = start; r < kN; r++)
            xv = rn_add(xv, rn_mul(m.x[r], m.vec[r]));
        /* `-0.5 * tau_i * xv` in fp64 rounds once to this fp32 value. */
        const float half_tau = rn_mul(-0.5f, tau);
        m.scalar[1] = rn_mul(half_tau, xv);
    }
    __syncthreads();
    const float alpha = m.scalar[1];
    for (uint32_t r = start + lid; r < kN; r += blockDim.x)
        m.x[r] = rn_add(m.x[r], rn_mul(alpha, m.vec[r]));
    __syncthreads();
}

/* One step of convert_to_tridiagonal(), speed.c. */
__device__ void tridiagonal_step(const Slm &m, uint32_t i)
{
    const uint32_t lid = threadIdx.x;
    const uint32_t start = i + 1u;
    if (lid == 0u)
        householder_scalars(m, i, start);
    __syncthreads();
    householder_scale(m, i, start);
    const float tau = m.scalar[0];
    if (lid == 0u) {
        for (uint32_t j = start; j < kN; j++)
            m.vec[j] = m.a[j * kN + i];
        if (tau != 0.0f) {
            m.a[start * kN + i] = m.vec[start];
            m.vec[start] = 1.0f;
        }
    }
    __syncthreads();
    if (tau == 0.0f)
        return;
    tridiagonal_vectors(m, start, tau);
    const uint32_t span = kN - start;
    for (uint32_t idx = lid; idx < span * span; idx += blockDim.x) {
        const uint32_t r = start + idx / span;
        const uint32_t c = start + idx % span;
        const float p1 = rn_mul(m.x[r], m.vec[c]);
        const float p2 = rn_mul(m.vec[r], m.x[c]);
        m.a[r * kN + c] = rn_sub(m.a[r * kN + c], rn_add(p1, p2));
    }
    __syncthreads();
}

/* chop_small_elements(), speed.c. */
__device__ void chop_small(const float *d, float *sd, uint32_t n)
{
    for (uint32_t i = 0; i + 1u < n; i++) {
        const float bound = rn_add(fabsf(d[i]), fabsf(d[i + 1u]));
        if (below_eps_scaled(fabsf(sd[i]), bound))
            sd[i] = 0.0f;
    }
}

/* trailing_eigenvalue(), speed.c. */
__device__ float trailing_eigenvalue(const float *d, const float *sd, uint32_t n)
{
    const float ta = d[n - 2u];
    const float tb = d[n - 1u];
    const float tab = sd[n - 2u];
    const float dt = rn_div(rn_sub(ta, tb), 2.0f);
    if (dt > 0.0f) {
        const float den = rn_add(dt, pythagoras(dt, tab));
        return rn_sub(tb, rn_mul(tab, rn_div(tab, den)));
    }
    if (dt == 0.0f)
        return rn_sub(tb, fabsf(tab));
    const float den = rn_add(-dt, pythagoras(dt, tab));
    return rn_add(tb, rn_mul(tab, rn_div(tab, den)));
}

/* create_givens(), speed.c. */
__device__ void create_givens(float a, float b, float &c, float &s)
{
    if (b == 0.0f) {
        c = 1.0f;
        s = 0.0f;
        return;
    }
    const bool b_larger = fabsf(b) > fabsf(a);
    const float t = b_larger ? rn_div(-a, b) : rn_div(-b, a);
    /* `1.0 / sqrt(1 + t * t)`: an fp64 root and quotient rounded to fp32. */
    const float unit = speed_givens_unit(rn_add(1.0f, rn_mul(t, t)));
    const float other = rn_mul(unit, t);
    s = b_larger ? unit : other;
    c = b_larger ? other : unit;
}

struct Rotated {
    float ak;
    float bk;
    float ap;
};

__device__ __forceinline__ Rotated make_rotated(float ak, float bk, float ap)
{
    Rotated r;
    r.ak = ak;
    r.bk = bk;
    r.ap = ap;
    return r;
}

/* G' T G of qr_step_size2() / qr_step_general(), speed.c. */
__device__ __forceinline__ Rotated rotate(float c, float s, float ap, float bp, float aq)
{
    const float cap = rn_mul(c, ap);
    const float sbp = rn_mul(s, bp);
    const float saq = rn_mul(s, aq);
    const float cbp = rn_mul(c, bp);
    const float sap = rn_mul(s, ap);
    const float caq = rn_mul(c, aq);
    const float l1 = rn_mul(c, rn_sub(cap, sbp));
    const float l2 = rn_mul(s, rn_sub(saq, cbp));
    const float m1 = rn_mul(c, rn_add(sap, cbp));
    const float m2 = rn_mul(s, rn_add(sbp, caq));
    const float n1 = rn_mul(s, rn_add(sap, cbp));
    const float n2 = rn_mul(c, rn_add(sbp, caq));
    return make_rotated(rn_add(l1, l2), rn_sub(m1, m2), rn_add(n1, n2));
}

__device__ void qr_step_size2(float *d, float *sd, float x, float z)
{
    float c = 0.0f;
    float s = 0.0f;
    create_givens(x, z, c, s);
    const Rotated r = rotate(c, s, d[0], sd[0], d[1]);
    d[0] = r.ak;
    sd[0] = r.bk;
    d[1] = r.ap;
}

__device__ void qr_step_general(float *d, float *sd, uint32_t n, float x, float z)
{
    float bk = 0.0f;
    float zk = 0.0f;
    float ap = d[0];
    float bp = sd[0];
    float aq = d[1];
    float bq = sd[1];
    for (uint32_t k = 0; k + 1u < n; k++) {
        float c = 0.0f;
        float s = 0.0f;
        create_givens(x, z, c, s);
        const float bk1 = rn_sub(rn_mul(c, bk), rn_mul(s, zk));
        const Rotated r = rotate(c, s, ap, bp, aq);
        const float zp1 = rn_mul(-s, bq);
        const float bq1 = rn_mul(c, bq);
        bk = r.bk;
        zk = zp1;
        ap = r.ap;
        bp = bq1;
        if (k + 2u < n)
            aq = d[k + 2u];
        if (k + 3u < n)
            bq = sd[k + 2u];
        d[k] = r.ak;
        if (k > 0u)
            sd[k - 1u] = bk1;
        if (k + 2u < n)
            sd[k + 1u] = bp;
        x = bk;
        z = zk;
    }
    d[n - 1u] = ap;
    sd[n - 2u] = bk;
}

/* qr_step(), speed.c. */
__device__ void qr_step(float *d, float *sd, uint32_t n)
{
    float mu = trailing_eigenvalue(d, sd, n);
    const float scale = rn_add(fabsf(d[0]), fabsf(sd[0]));
    if (below_eps_scaled(scale, fabsf(mu)))
        mu = 0.0f;
    const float x = rn_sub(d[0], mu);
    const float z = sd[0];
    if (n == 2u) {
        qr_step_size2(d, sd, x, z);
        return;
    }
    qr_step_general(d, sd, n, x, z);
}

/* compute_eigenvalues_tridiagonal(), speed.c. Returns whether the iteration
 * cap was reached. */
__device__ bool eigenvalues_tridiagonal(float *d, float *sd)
{
    chop_small(d, sd, kN);
    uint32_t b = kN - 1u;
    uint32_t iter = 0u;
    for (uint32_t guard = 0; guard < kQrIterationCap + kN && b > 0u && iter < kQrIterationCap;
         guard++) {
        if (sd[b - 1u] == 0.0f) {
            b--;
            continue;
        }
        uint32_t a = b - 1u;
        for (uint32_t probe = 0; probe < kN && a > 0u && sd[a - 1u] != 0.0f; probe++)
            a--;
        const uint32_t n_block = b - a + 1u;
        qr_step(d + a, sd + a, n_block);
        chop_small(d + a, sd + a, n_block);
        iter++;
    }
    return iter == kQrIterationCap;
}

/* Tridiagonal QR on thread 0; eigenvalues, regularity and the cap flag. */
__device__ void eigen_serial(const Slm &m, float *eig)
{
    for (uint32_t r = 0; r < kN; r++)
        m.d[r] = m.a[r * kN + r];
    for (uint32_t r = 0; r + 1u < kN; r++)
        m.sd[r] = m.a[(r + 1u) * kN + r];
    const bool capped = eigenvalues_tridiagonal(m.d, m.sd);
    bool regular = true;
    for (uint32_t r = 0; r < kN; r++) {
        eig[r] = m.d[r];
        if (below_eps(m.d[r])) /* is_matrix_regular() */
            regular = false;
    }
    m.scalar[3] = regular ? 1.0f : 0.0f;
    m.scalar[4] = capped ? 1.0f : 0.0f;
}

/* matrix_mul() with speed_matmul_scalar() accumulation order. */
__device__ void block_matmul(float *dst, const float *x, const float *y)
{
    for (uint32_t idx = threadIdx.x; idx < kMatrix; idx += blockDim.x) {
        const uint32_t i = idx / kN;
        const uint32_t j = idx % kN;
        float acc = 0.0f;
        for (uint32_t k = 0; k < kN; k++)
            acc = rn_add(acc, rn_mul(x[i * kN + k], y[k * kN + j]));
        dst[idx] = acc;
    }
    __syncthreads();
}

/* vector_norm(), speed.c. */
__device__ float vector_norm(const float *v)
{
    float sum = 0.0f;
    for (uint32_t i = 0; i < kN; i++)
        sum = rn_add(sum, rn_mul(v[i], v[i]));
    return rn_sqrt(sum);
}

/* matrix_minor() of the working matrix, then its k-th column into m.vec. */
__device__ void qr_minor_column(const Slm &m, uint32_t k)
{
    const uint32_t lid = threadIdx.x;
    for (uint32_t idx = lid; idx < kMatrix; idx += blockDim.x) {
        const uint32_t i = idx / kN;
        const uint32_t j = idx % kN;
        if (i < k || j < k)
            m.z[idx] = i == j ? 1.0f : 0.0f;
    }
    __syncthreads();
    for (uint32_t r = lid; r < kN; r += blockDim.x)
        m.vec[r] = m.z[r * kN + k];
    __syncthreads();
}

/* One column of matrix_qr_decomposition(): minor, reflector vector and
 * I - 2 v v^T. Returns false when the reflector vanishes; the norm is read
 * after a barrier, so every thread returns the same value. */
__device__ bool qr_reflector(const Slm &m, uint32_t k)
{
    const uint32_t lid = threadIdx.x;
    qr_minor_column(m, k);
    if (lid == 0u) {
        const float norm = vector_norm(m.vec);
        const float shift = rn_mul(sign_of(m.cov[k * kN + k]), norm);
        m.vec[k] = rn_add(m.vec[k], shift);
        m.scalar[2] = vector_norm(m.vec);
    }
    __syncthreads();
    const float vn = m.scalar[2];
    if (vn == 0.0f)
        return false;
    for (uint32_t r = lid; r < kN; r += blockDim.x)
        m.vec[r] = rn_div(m.vec[r], vn);
    __syncthreads();
    for (uint32_t idx = lid; idx < kMatrix; idx += blockDim.x) {
        const uint32_t i = idx / kN;
        const uint32_t j = idx % kN;
        const float value = rn_mul(rn_mul(-2.0f, m.vec[i]), m.vec[j]);
        m.h[idx] = i == j ? rn_add(value, 1.0f) : value;
    }
    __syncthreads();
    return true;
}

/* matrix_qr_decomposition(), speed.c: leaves the accumulated reflector product
 * (the matrix solve_linear_system() multiplies B by) in m.q and R in m.t. */
__device__ void qr_decompose(Slm &m)
{
    for (uint32_t idx = threadIdx.x; idx < kMatrix; idx += blockDim.x) {
        m.z[idx] = m.cov[idx];
        m.q[idx] = (idx / kN == idx % kN) ? 1.0f : 0.0f;
    }
    __syncthreads();
    for (uint32_t k = 0; k + 1u < kN; k++) {
        if (!qr_reflector(m, k))
            continue;
        block_matmul(m.t, m.h, m.z);
        float *swap = m.z;
        m.z = m.t;
        m.t = swap;
        block_matmul(m.t, m.h, m.q);
        swap = m.q;
        m.q = m.t;
        m.t = swap;
    }
    block_matmul(m.t, m.q, m.cov);
}

/* solve_triangular_system()'s pivot rejection, speed.c. */
__device__ bool pivot_singular(const float *r)
{
    bool singular = false;
    for (uint32_t i = 0; i < kN; i++) {
        if (below_eps(fabsf(r[i * kN + i])))
            singular = true;
    }
    return singular;
}

__device__ void linalg_store(const SpeedCudaFrameArgs &a, const Slm &m, uint32_t ch, bool regular)
{
    const uint32_t lid = threadIdx.x;
    float *qmat = VMAF_CUDA_DPTR(float, a.qmat) + static_cast<size_t>(ch) * kMatrix;
    float *rmat = VMAF_CUDA_DPTR(float, a.rmat) + static_cast<size_t>(ch) * kMatrix;
    if (regular) {
        for (uint32_t idx = lid; idx < kMatrix; idx += blockDim.x) {
            qmat[idx] = m.q[idx];
            rmat[idx] = m.t[idx];
        }
    }
    if (lid == 0u) {
        int32_t *status = VMAF_CUDA_DPTR(int32_t, a.status) + static_cast<size_t>(ch) * 2u;
        const bool singular = !regular || pivot_singular(m.t);
        status[0] = singular ? 1 : 0;
        status[1] = m.scalar[4] != 0.0f ? 1 : 0;
    }
}

/* ------------------------------------------------------------------ */
/* Solve and variance per block: solve_linear_system() tail,           */
/* compute_pointwise_product_and_division(), sum_columns()             */
/* ------------------------------------------------------------------ */

__device__ void solve_block(const float *__restrict__ b, uint32_t stride,
                            const float *__restrict__ q, const float *__restrict__ r,
                            float *solution)
{
    float y[kN];
    for (uint32_t i = 0; i < kN; i++) {
        float acc = 0.0f;
        for (uint32_t k = 0; k < kN; k++)
            acc = rn_add(acc, rn_mul(q[i * kN + k], b[static_cast<size_t>(k) * stride]));
        y[i] = acc;
    }
    for (uint32_t step = 0; step < kN; step++) {
        const uint32_t i = kN - 1u - step;
        float term = y[i];
        for (uint32_t k = i + 1u; k < kN; k++)
            term = rn_sub(term, rn_mul(solution[k], r[i * kN + k]));
        solution[i] = rn_div(term, r[i * kN + i]);
    }
}

/* ------------------------------------------------------------------ */
/* Kernels                                                             */
/* ------------------------------------------------------------------ */

} // namespace

extern "C" {

/* vif_scale_frame_s() of every channel: channels x scaled_h x scaled_w. */
__global__ void __launch_bounds__(SPEED_CUDA_PIXEL_THREADS)
    speed_scale_kernel(const SpeedCudaFrameArgs a)
{
    const SpeedGpuGeometry &g = a.geometry;
    const size_t plane = static_cast<size_t>(g.scaled_w) * g.scaled_h;
    const size_t idx = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (idx >= plane * a.channels)
        return;
    const auto ch = static_cast<uint32_t>(idx / plane);
    const size_t rest = idx % plane;
    const auto y = static_cast<uint32_t>(rest / g.scaled_w);
    const auto x = static_cast<uint32_t>(rest % g.scaled_w);
    float *dst = VMAF_CUDA_DPTR(float, a.scaled);
    const float *lanczos = VMAF_CUDA_DPTR(const float, a.lanczos);
    if (g.bytes_per_sample == 2u) {
        dst[idx] = scale_sample(RawSource<uint16_t>(a), g, lanczos, ch, y, x);
    } else {
        dst[idx] = scale_sample(RawSource<uint8_t>(a), g, lanczos, ch, y, x);
    }
}

/* Anti-alias filter at the decimated points of the raw planes (no prescale):
 * channels x down_h x down_w. */
__global__ void __launch_bounds__(SPEED_CUDA_PIXEL_THREADS)
    speed_decimate_raw_kernel(const SpeedCudaFrameArgs a)
{
    const SpeedGpuGeometry &g = a.geometry;
    const size_t plane = static_cast<size_t>(g.down_w) * g.down_h;
    const size_t idx = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (idx >= plane * a.channels)
        return;
    const auto ch = static_cast<uint32_t>(idx / plane);
    const size_t rest = idx % plane;
    const auto i = static_cast<uint32_t>(rest / g.down_w);
    const auto j = static_cast<uint32_t>(rest % g.down_w);
    const float *taps = VMAF_CUDA_DPTR(const float, a.taps);
    float *dst = VMAF_CUDA_DPTR(float, a.down);
    const uint32_t width = a.antialias_width;
    if (g.bytes_per_sample == 2u) {
        dst[idx] =
            antialias_at(RawSource<uint16_t>(a), taps, width, g.scaled_w, g.scaled_h, ch, i, j);
    } else {
        dst[idx] =
            antialias_at(RawSource<uint8_t>(a), taps, width, g.scaled_w, g.scaled_h, ch, i, j);
    }
}

/* Anti-alias filter at the decimated points of the prescaled planes. */
__global__ void __launch_bounds__(SPEED_CUDA_PIXEL_THREADS)
    speed_decimate_scaled_kernel(const SpeedCudaFrameArgs a)
{
    const SpeedGpuGeometry &g = a.geometry;
    const size_t plane = static_cast<size_t>(g.down_w) * g.down_h;
    const size_t idx = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (idx >= plane * a.channels)
        return;
    const auto ch = static_cast<uint32_t>(idx / plane);
    const size_t rest = idx % plane;
    const auto i = static_cast<uint32_t>(rest / g.down_w);
    const auto j = static_cast<uint32_t>(rest % g.down_w);
    const FloatSource scaled(VMAF_CUDA_DPTR(const float, a.scaled), g.scaled_w, g.scaled_h);
    float *dst = VMAF_CUDA_DPTR(float, a.down);
    dst[idx] = antialias_at(scaled, VMAF_CUDA_DPTR(const float, a.taps), a.antialias_width,
                            g.scaled_w, g.scaled_h, ch, i, j);
}

/* Local mean subtraction and the independent term: channels x trunc_h x
 * trunc_w. */
__global__ void __launch_bounds__(SPEED_CUDA_PIXEL_THREADS)
    speed_centre_kernel(const SpeedCudaFrameArgs a)
{
    const SpeedGpuGeometry &g = a.geometry;
    const size_t plane_size = static_cast<size_t>(g.trunc_w) * g.trunc_h;
    const size_t idx = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (idx >= plane_size * a.channels)
        return;
    const auto ch = static_cast<uint32_t>(idx / plane_size);
    const size_t rest = idx % plane_size;
    const auto i = static_cast<uint32_t>(rest / g.trunc_w);
    const auto j = static_cast<uint32_t>(rest % g.trunc_w);
    const float *plane =
        VMAF_CUDA_DPTR(const float, a.down) + static_cast<size_t>(ch) * g.down_h * g.down_w;
    const float *taps = VMAF_CUDA_DPTR(const float, a.taps) + kMaxTaps;
    const float smooth = lowpass_at(plane, taps, a.lowpass_width, g, i, j);
    const float value = rn_sub(plane[static_cast<size_t>(i) * g.down_w + j], smooth);
    VMAF_CUDA_DPTR(float, a.centered)[idx] = value;
    const uint32_t element = (i % kBlock) * kBlock + (j % kBlock);
    const uint32_t tile = (i / kBlock) * g.blocks_h + (j / kBlock);
    const size_t term_size = static_cast<size_t>(kN) * g.blocks;
    float *indterm = VMAF_CUDA_DPTR(float, a.indterm);
    indterm[ch * term_size + static_cast<size_t>(element) * g.blocks + tile] = value;
}

/* The 25 submatrix means of every channel, one sequential sum per thread. */
__global__ void __launch_bounds__(SPEED_CUDA_MEANS_THREADS)
    speed_means_kernel(const SpeedCudaFrameArgs a)
{
    const SpeedGpuGeometry &g = a.geometry;
    const uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= a.channels * kN)
        return;
    const uint32_t ch = idx / kN;
    const uint32_t element = idx % kN;
    const float *plane =
        VMAF_CUDA_DPTR(const float, a.centered) + static_cast<size_t>(ch) * g.trunc_h * g.trunc_w;
    const uint32_t row0 = element / kBlock;
    const uint32_t col0 = element % kBlock;
    float sum = 0.0f;
    for (uint32_t i = 0; i < g.sub_h; i++)
        sum = add_row(sum, plane + static_cast<size_t>(row0 + i) * g.trunc_w + col0, g.sub_w);
    const auto count = static_cast<float>(g.sub_w * g.sub_h);
    VMAF_CUDA_DPTR(float, a.means)[idx] = rn_div(sum, count);
}

/* One block per (channel, lower-triangle entry); blockDim.x is a power of
 * two <= SPEED_CUDA_COV_MAX_THREADS. */
__global__ void __launch_bounds__(SPEED_CUDA_COV_MAX_THREADS)
    speed_covariance_kernel(const SpeedCudaFrameArgs a)
{
    __shared__ float s_hi[SPEED_CUDA_COV_MAX_THREADS];
    __shared__ float s_lo[SPEED_CUDA_COV_MAX_THREADS];
    const SpeedGpuGeometry &g = a.geometry;
    const uint32_t group = blockIdx.x;
    const uint32_t ch = group / kTriangle;
    uint32_t x = 0u;
    uint32_t y = 0u;
    triangle_entry(group % kTriangle, x, y);
    const uint32_t lid = threadIdx.x;
    const uint32_t threads = blockDim.x;
    const float *plane =
        VMAF_CUDA_DPTR(const float, a.centered) + static_cast<size_t>(ch) * g.trunc_h * g.trunc_w;
    const float *means = VMAF_CUDA_DPTR(const float, a.means) + static_cast<size_t>(ch) * kN;
    const Ff part = covariance_partial(plane, g, x, y, means[x], means[y], lid, threads);
    s_hi[lid] = part.hi;
    s_lo[lid] = part.lo;
    __syncthreads();
    for (uint32_t span = threads / 2u; span > 0u; span >>= 1u) {
        if (lid < span) {
            const Ff merged =
                ff_add(make_ff(s_hi[lid], s_lo[lid]), make_ff(s_hi[lid + span], s_lo[lid + span]));
            s_hi[lid] = merged.hi;
            s_lo[lid] = merged.lo;
        }
        __syncthreads();
    }
    if (lid == 0u) {
        const float value = ff_div_to_float(make_ff(s_hi[0], s_lo[0]), count_ff(g.sub_w * g.sub_h));
        float *matrix = VMAF_CUDA_DPTR(float, a.cov) + static_cast<size_t>(ch) * kMatrix;
        matrix[x * kN + y] = value;
        matrix[y * kN + x] = value;
    }
}

/* Eigenvalues, regularity, QR factorisation: one block of
 * SPEED_CUDA_LINALG_THREADS per channel. The reference and distorted sides
 * are separate channels, so each keeps its own eigenbasis. */
__global__ void __launch_bounds__(SPEED_CUDA_LINALG_THREADS)
    speed_linalg_kernel(const SpeedCudaFrameArgs a)
{
    __shared__ float slm[kSlmFloats];
    Slm m = slm_layout(slm);
    const uint32_t ch = blockIdx.x;
    const uint32_t lid = threadIdx.x;
    const float *cov = VMAF_CUDA_DPTR(const float, a.cov) + static_cast<size_t>(ch) * kMatrix;
    for (uint32_t idx = lid; idx < kMatrix; idx += blockDim.x) {
        m.a[idx] = cov[idx];
        m.cov[idx] = cov[idx];
    }
    __syncthreads();
    for (uint32_t i = 0; i + 2u < kN; i++)
        tridiagonal_step(m, i);
    if (lid == 0u)
        eigen_serial(m, VMAF_CUDA_DPTR(float, a.eig) + static_cast<size_t>(ch) * kN);
    __syncthreads();
    const bool regular = m.scalar[3] != 0.0f;
    if (regular)
        qr_decompose(m);
    linalg_store(a, m, ch, regular);
}

/* Solve and variance of every (channel, block). A singular channel keeps its
 * zero solution, as solve_covariance_system() does. The variances, the
 * eigenvalues and the status words are the block the host reads back; its
 * tail forms the entropies and the score (ADR-1477). */
__global__ void __launch_bounds__(SPEED_CUDA_SOLVE_THREADS)
    speed_solve_kernel(const SpeedCudaFrameArgs a)
{
    const SpeedGpuGeometry &g = a.geometry;
    const uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= a.channels * g.blocks)
        return;
    const uint32_t ch = idx / g.blocks;
    const uint32_t block = idx % g.blocks;
    const float *b =
        VMAF_CUDA_DPTR(const float, a.indterm) + static_cast<size_t>(ch) * kN * g.blocks + block;
    float solution[kN];
    for (float &element : solution)
        element = 0.0f;
    const int32_t *status = VMAF_CUDA_DPTR(const int32_t, a.status);
    if (status[static_cast<size_t>(ch) * 2u] == 0) {
        const size_t matrix = static_cast<size_t>(ch) * kMatrix;
        solve_block(b, g.blocks, VMAF_CUDA_DPTR(const float, a.qmat) + matrix,
                    VMAF_CUDA_DPTR(const float, a.rmat) + matrix, solution);
    }
    float variance = rn_div(rn_mul(solution[0], b[0]), kElementsF);
    for (uint32_t e = 1; e < kN; e++) {
        const float product = rn_mul(solution[e], b[static_cast<size_t>(e) * g.blocks]);
        variance = rn_add(variance, rn_div(product, kElementsF));
    }
    VMAF_CUDA_DPTR(float, a.var)[idx] = variance;
}

} /* extern "C" */
