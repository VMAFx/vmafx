/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Device-resident SpEED pipeline (ADR-1358). See speed_sycl_pipeline.h for
 *  the stage list and the numerical contract.
 *
 *  Every device routine below is a line-for-line port of its CPU reference
 *  in speed.c / vif_tools.c / convolution_internal.h, named in the comment
 *  above it. Products are held in named temporaries so the source never asks
 *  for a fused multiply-add, and the TU is compiled with contraction off, so
 *  each multiply and each add rounds exactly where the host rounds. The
 *  reference evaluates a few expressions in fp64 (the covariance sum, the
 *  EIGENVALUE_EPS comparisons, the prescale sample coordinates); those are
 *  reproduced with exact fp32 pairs rather than fp64, which the device
 *  contract forbids (ADR-0220). This file must not mention the fp64 type at
 *  all: core/test/test_sycl_kernel_source_contract.py enforces that.
 */

#include "speed_sycl_pipeline.h"

#include <sycl/sycl.hpp>

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <numbers>
#include <utility>

#include "log.h"
#include "sycl_exact_fp.h"

using speed_sycl::ChannelBinding;
using speed_sycl::FrameResult;
using speed_sycl::Geometry;
using speed_sycl::kBlock;
using speed_sycl::kElements;
using speed_sycl::kMaxChannels;
using speed_sycl::kMaxPairs;
using speed_sycl::kMaxRawPlanes;
using speed_sycl::kMaxTaps;
using speed_sycl::Pipeline;
using speed_sycl::PipelineConfig;
using speed_sycl::Scoring;

namespace
{

constexpr uint32_t kN = kElements;                   /* 25 */
constexpr uint32_t kMatrix = kN * kN;                /* 625 */
constexpr uint32_t kTriangle = kN * (kN + 1u) / 2u;  /* 325 */
constexpr uint32_t kGroup = 256u;                    /* work-group size */
constexpr uint32_t kLinalgGroup = 64u;               /* 25x25 linear algebra */
constexpr uint32_t kDecimation = 16u;                /* 2^NUM_SCALES */
constexpr uint32_t kQrIterationCap = 500u;           /* EIGENVALUE_MAX_ITERS */
constexpr float kPictureOffset = -128.0f;            /* picture_copy() offset */
constexpr float kElementsF = static_cast<float>(kN); /* elements_in_block */
constexpr float kEpsHi = 0x1.0c6f7ap-20f;            /* (float)EIGENVALUE_EPS */
constexpr float kEpsLo = 0x1.6bdb1ap-49f;            /* EIGENVALUE_EPS - kEpsHi, exact */

/* Correctly rounded division / square root and exact fp32 pairs, shared with
 * the ssimulacra2 twin (sycl_exact_fp.h, ADR-1363). */
using vmaf_sycl_exact::div_rn;
using vmaf_sycl_exact::Ff;
using vmaf_sycl_exact::ff_add;
using vmaf_sycl_exact::ff_div_to_float;
using vmaf_sycl_exact::ff_mul;
using vmaf_sycl_exact::quick_two_sum;
using vmaf_sycl_exact::sqrt_rn;
using vmaf_sycl_exact::two_prod;
using vmaf_sycl_exact::two_sum;

/* speed.c compares against EIGENVALUE_EPS = 1e-6, an fp64 constant, after
 * promoting the fp32 operands. `a < 1e-6 * s` is decided exactly here from
 * kEpsHi + kEpsLo == 1e-6 and two exact products. */
inline bool below_eps_scaled(float a, float s)
{
    const Ff major = two_prod(kEpsHi, s);
    const Ff minor = two_prod(kEpsLo, s);
    const float tail = (major.lo + minor.hi) + minor.lo;
    if (a < 0.5f * major.hi || a > 2.0f * major.hi) {
        return a < major.hi;
    }
    const float gap = a - major.hi; /* exact (Sterbenz) */
    return gap < tail;
}

} // namespace

namespace
{

/* `x < 1e-6` with fp64 promotion: no fp32 value lies strictly between
 * kEpsHi and 1e-6, so this is `x <= kEpsHi`. */
inline bool below_eps(float x)
{
    return x <= kEpsHi;
}

/* log2f() of the reference. The device builtin differs from the host libm in
 * about 9% of the arguments SpEED feeds it, so log2 is evaluated here in fp32
 * pairs to about 2^-45 and rounded once: correctly rounded except in the rare
 * cases a host log2f is not either. ln(m) = 2 atanh(s), s = (m - 1)/(m + 1),
 * m in [sqrt(1/2), sqrt(2)], |s| <= 0.1716; the series stops at s^21. */
constexpr float kSqrt2 = std::numbers::sqrt2_v<float>;
constexpr Ff kLog2e = {.hi = std::numbers::log2e_v<float>, .lo = 0x1.4ae0c0p-26f};
constexpr Ff kInverseOdd[10] = {
    {.hi = 0x1.555556p-2f, .lo = -0x1.555556p-27f}, /* 1/3 */
    {.hi = 0x1.99999ap-3f, .lo = -0x1.99999ap-29f}, /* 1/5 */
    {.hi = 0x1.24924ap-3f, .lo = -0x1.b6db6ep-28f}, /* 1/7 */
    {.hi = 0x1.c71c72p-4f, .lo = -0x1.c71c72p-31f}, /* 1/9 */
    {.hi = 0x1.745d18p-4f, .lo = -0x1.745d18p-29f}, /* 1/11 */
    {.hi = 0x1.3b13b2p-4f, .lo = -0x1.89d89ep-29f}, /* 1/13 */
    {.hi = 0x1.111112p-4f, .lo = -0x1.dddddep-29f}, /* 1/15 */
    {.hi = 0x1.e1e1e2p-5f, .lo = -0x1.e1e1e2p-33f}, /* 1/17 */
    {.hi = 0x1.af286cp-5f, .lo = -0x1.af286cp-32f}, /* 1/19 */
    {.hi = 0x1.861862p-5f, .lo = -0x1.e79e7ap-31f}, /* 1/21 */
};

inline Ff ln_mantissa(float m)
{
    const float num = m - 1.0f;      /* exact */
    const Ff den = two_sum(m, 1.0f); /* exact */
    const float s_hi = div_rn(num, den.hi);
    const float residual = sycl::fma(-s_hi, den.hi, num);
    const float tail = s_hi * den.lo;
    const float s_lo = div_rn(residual - tail, den.hi);
    const Ff s = quick_two_sum(s_hi, s_lo);
    const Ff u = ff_mul(s, s);
    Ff poly = kInverseOdd[9];
    for (uint32_t k = 9u; k > 0u; k--) {
        poly = ff_add(ff_mul(poly, u), kInverseOdd[k - 1u]);
    }
    poly = ff_add(ff_mul(poly, u), Ff{.hi = 1.0f, .lo = 0.0f});
    const Ff half = ff_mul(s, poly);
    return {.hi = 2.0f * half.hi, .lo = 2.0f * half.lo};
}

} // namespace

namespace
{

inline float speed_log2(float x)
{
    if (!(x > 0.0f)) {
        return x == 0.0f ? -std::numeric_limits<float>::infinity() :
                           std::numeric_limits<float>::quiet_NaN();
    }
    if (x == std::numeric_limits<float>::infinity()) {
        return x;
    }
    auto bits = sycl::bit_cast<uint32_t>(x);
    int32_t exponent = 0;
    if (bits < 0x00800000u) { /* subnormal */
        bits = sycl::bit_cast<uint32_t>(x * 0x1p23f);
        exponent = -23;
    }
    exponent += static_cast<int32_t>(bits >> 23u) - 127;
    float m = sycl::bit_cast<float>((bits & 0x007fffffu) | 0x3f800000u);
    if (m > kSqrt2) {
        m = m * 0.5f;
        exponent += 1;
    }
    const Ff log2m = ff_mul(ln_mantissa(m), kLog2e);
    return ff_add(Ff{.hi = static_cast<float>(exponent), .lo = 0.0f}, log2m).hi;
}

/* ------------------------------------------------------------------ */
/* Picture sources                                                     */
/* ------------------------------------------------------------------ */

/* convolution_reflect101(), convolution_internal.h. */
inline uint32_t reflect101(int32_t index, uint32_t size)
{
    const int32_t n = static_cast<int32_t>(size);
    if (n <= 1) {
        return 0u;
    }
    int32_t folded = index;
    for (uint32_t bounce = 0; bounce < 8u && (folded < 0 || folded >= n); bounce++) {
        folded = folded < 0 ? -folded : 2 * n - folded - 2;
    }
    return static_cast<uint32_t>(folded);
}

/* Whether every tap of a `2 * radius + 1` window around `centre` is inside
 * [0, size), so the reflect-101 fold can be skipped. */
inline bool window_inside(int32_t centre, int32_t radius, uint32_t size)
{
    return centre >= radius && centre + radius < static_cast<int32_t>(size);
}

} // namespace

namespace
{

/* Plane index of tap `k` around `centre`. */
inline uint32_t tap(int32_t centre, int32_t radius, uint32_t k, uint32_t size, bool inside)
{
    const int32_t index = centre - radius + static_cast<int32_t>(k);
    return inside ? static_cast<uint32_t>(index) : reflect101(index, size);
}

struct RawPlanes {
    const void *minuend[kMaxChannels];
    const void *subtrahend[kMaxChannels];
    uint32_t width;
    float scale;
};

/* picture_copy(): 8-bit `(float)v + offset`, else `(float)v / scaler + offset`. */
template <typename T> inline float picture_value(const void *plane, size_t offset, float scale)
{
    const float sample = static_cast<float>(static_cast<const T *>(plane)[offset]);
    if constexpr (sizeof(T) == 1u) {
        (void)scale;
        return sample + kPictureOffset;
    } else {
        const float scaled = div_rn(sample, scale);
        return scaled + kPictureOffset;
    }
}

/* Converted picture, or the temporal difference `minuend - subtrahend` of two
 * converted pictures (speed.c extract(): subtract_image()). */
template <typename T> class RawSource
{
  public:
    explicit RawSource(const RawPlanes &planes) : planes_(planes)
    {
    }

    float operator()(uint32_t channel, uint32_t row, uint32_t col) const
    {
        const size_t offset = static_cast<size_t>(row) * planes_.width + col;
        const float value = picture_value<T>(planes_.minuend[channel], offset, planes_.scale);
        if (planes_.subtrahend[channel] == nullptr) {
            return value;
        }
        const float other = picture_value<T>(planes_.subtrahend[channel], offset, planes_.scale);
        return value - other;
    }

  private:
    RawPlanes planes_;
};

} // namespace

namespace
{

/* A materialised float plane per channel, row stride `width`. */
class FloatSource
{
  public:
    FloatSource(const float *plane, uint32_t width, uint32_t height)
        : plane_(plane), width_(width), height_(height)
    {
    }

    float operator()(uint32_t channel, uint32_t row, uint32_t col) const
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

struct ScaleArgs {
    float *dst; /* channels x dst_h x dst_w */
    uint32_t src_w;
    uint32_t src_h;
    uint32_t dst_w;
    uint32_t dst_h;
    int32_t method; /* enum vif_scaling_method */
};

/* `(y + 0.5) * ratio - 0.5`, evaluated by the reference in fp64 and rounded
 * once to fp32. Every intermediate below is exact. */
inline float centre_coordinate(uint32_t index, float ratio)
{
    const float centre = static_cast<float>(index) + 0.5f;
    const Ff scaled = two_prod(centre, ratio);
    const Ff shifted = two_sum(scaled.hi, -0.5f);
    const float tail = shifted.lo + scaled.lo;
    return shifted.hi + tail;
}

} // namespace

namespace
{

/* mirror(), vif_tools.c (float arguments). */
inline float mirror_coordinate(float i, float right)
{
    if (i < 0.0f) {
        return -i;
    }
    if (i > right) {
        const float twice = 2.0f * right;
        return twice - i;
    }
    return i;
}

template <class Source>
inline float scale_bilinear(const Source &src, uint32_t ch, const ScaleArgs &a, float x, float y)
{
    const float right = static_cast<float>(a.src_w - 1u);
    const float bottom = static_cast<float>(a.src_h - 1u);
    const int32_t x1 = static_cast<int32_t>(mirror_coordinate(sycl::floor(x), right));
    const int32_t x2 = static_cast<int32_t>(mirror_coordinate(sycl::ceil(x), right));
    const int32_t y1 = static_cast<int32_t>(mirror_coordinate(sycl::floor(y), bottom));
    const int32_t y2 = static_cast<int32_t>(mirror_coordinate(sycl::ceil(y), bottom));
    const float dx = x - static_cast<float>(x1);
    const float dy = y - static_cast<float>(y1);
    const float ix = 1.0f - dx;
    const float iy = 1.0f - dy;
    const float w11 = iy * ix;
    const float w12 = iy * dx;
    const float w21 = dy * ix;
    const float w22 = dy * dx;
    const float t11 = w11 * src(ch, static_cast<uint32_t>(y1), static_cast<uint32_t>(x1));
    const float t12 = w12 * src(ch, static_cast<uint32_t>(y1), static_cast<uint32_t>(x2));
    const float t21 = w21 * src(ch, static_cast<uint32_t>(y2), static_cast<uint32_t>(x1));
    const float t22 = w22 * src(ch, static_cast<uint32_t>(y2), static_cast<uint32_t>(x2));
    const float s1 = t11 + t12;
    const float s2 = s1 + t21;
    return s2 + t22;
}

} // namespace

namespace
{

/* bicubic_kernel(), vif_tools.c. */
inline float bicubic_weight(float t)
{
    const float a = -0.75f;
    const float u = t < 0.0f ? -t : t;
    if (u < 1.0f) {
        const float c1 = (a + 2.0f) * u;
        const float c2 = c1 - (a + 3.0f);
        const float c3 = c2 * u;
        const float c4 = c3 * u;
        return c4 + 1.0f;
    }
    if (u < 2.0f) {
        const float c1 = (u - 5.0f) * u;
        const float c2 = (c1 + 8.0f) * u;
        return (c2 - 4.0f) * a;
    }
    return 0.0f;
}

template <class Source>
inline float scale_bicubic(const Source &src, uint32_t ch, const ScaleArgs &a, float x, float y)
{
    const int32_t x0 = static_cast<int32_t>(sycl::floor(x));
    const int32_t y0 = static_cast<int32_t>(sycl::floor(y));
    const float dx = x - static_cast<float>(x0);
    const float dy = y - static_cast<float>(y0);
    float wx[4];
    float wy[4];
    for (int32_t i = -1; i <= 2; i++) {
        wx[i + 1] = bicubic_weight(static_cast<float>(i) - dx);
        wy[i + 1] = bicubic_weight(static_cast<float>(i) - dy);
    }
    const float right = static_cast<float>(a.src_w - 1u);
    const float bottom = static_cast<float>(a.src_h - 1u);
    float value = 0.0f;
    for (int32_t j = -1; j <= 2; j++) {
        for (int32_t i = -1; i <= 2; i++) {
            const auto xi =
                static_cast<int32_t>(mirror_coordinate(static_cast<float>(x0 + i), right));
            const auto yj =
                static_cast<int32_t>(mirror_coordinate(static_cast<float>(y0 + j), bottom));
            const float weight = wx[i + 1] * wy[j + 1];
            const float term =
                src(ch, static_cast<uint32_t>(yj), static_cast<uint32_t>(xi)) * weight;
            value = value + term;
        }
    }
    return value;
}

} // namespace

namespace
{

/* lanczos4_kernel(), vif_tools.c. The reference evaluates the weight in fp64
 * with sin(); fp32 sinpi() matches it to a few ulp, not bit for bit. */
inline float lanczos_weight(float x)
{
    const float a = 4.0f;
    if (x == 0.0f) {
        return 1.0f;
    }
    if (x > -a && x < a) {
        const float pi = std::numbers::pi_v<float>;
        const float s1 = sycl::sinpi(x);
        const float s2 = sycl::sinpi(div_rn(x, a));
        const float num = (a * s1) * s2;
        const float den = ((pi * pi) * x) * x;
        return div_rn(num, den);
    }
    return 0.0f;
}

template <class Source>
inline float scale_lanczos(const Source &src, uint32_t ch, const ScaleArgs &a, float x, float y)
{
    const int32_t x0 = static_cast<int32_t>(sycl::floor(x));
    const int32_t y0 = static_cast<int32_t>(sycl::floor(y));
    const float dx = x - static_cast<float>(x0);
    const float dy = y - static_cast<float>(y0);
    float wx[9];
    float wy[9];
    for (int32_t i = -4; i <= 4; i++) {
        wx[i + 4] = lanczos_weight(static_cast<float>(i) - dx);
        wy[i + 4] = lanczos_weight(static_cast<float>(i) - dy);
    }
    const float right = static_cast<float>(a.src_w - 1u);
    const float bottom = static_cast<float>(a.src_h - 1u);
    float value = 0.0f;
    float weight_sum = 0.0f;
    for (int32_t iy = -4; iy <= 4; iy++) {
        for (int32_t ix = -4; ix <= 4; ix++) {
            const float weight = wx[ix + 4] * wy[iy + 4];
            weight_sum = weight_sum + weight;
            const auto xi =
                static_cast<int32_t>(mirror_coordinate(static_cast<float>(x0 + ix), right));
            const auto yi =
                static_cast<int32_t>(mirror_coordinate(static_cast<float>(y0 + iy), bottom));
            const float term =
                src(ch, static_cast<uint32_t>(yi), static_cast<uint32_t>(xi)) * weight;
            value = value + term;
        }
    }
    return div_rn(value, weight_sum);
}

} // namespace

namespace
{

template <class Source>
inline float scale_sample(const Source &src, const ScaleArgs &a, uint32_t ch, uint32_t y,
                          uint32_t x)
{
    if (a.src_w == a.dst_w && a.src_h == a.dst_h) {
        return src(ch, y, x);
    }
    const float ratio_x = div_rn(static_cast<float>(a.src_w), static_cast<float>(a.dst_w));
    const float ratio_y = div_rn(static_cast<float>(a.src_h), static_cast<float>(a.dst_h));
    if (a.method == 0) { /* vif_scale_nearest */
        const auto sy = static_cast<uint32_t>(static_cast<float>(y) * ratio_y);
        const auto sx = static_cast<uint32_t>(static_cast<float>(x) * ratio_x);
        return src(ch, sy, sx);
    }
    const float xx = centre_coordinate(x, ratio_x);
    const float yy = centre_coordinate(y, ratio_y);
    if (a.method == 1) { /* vif_scale_bicubic */
        return scale_bicubic(src, ch, a, xx, yy);
    }
    if (a.method == 2) { /* vif_scale_lanczos4 */
        return scale_lanczos(src, ch, a, xx, yy);
    }
    return scale_bilinear(src, ch, a, xx, yy); /* vif_scale_bilinear */
}

template <class Source>
void launch_scale(sycl::queue &q, const Source &src, const ScaleArgs &args, uint32_t channels)
{
    const sycl::range<3> range(channels, args.dst_h, args.dst_w);
    q.parallel_for(range, [=](sycl::item<3> it) {
        const auto ch = static_cast<uint32_t>(it.get_id(0));
        const auto y = static_cast<uint32_t>(it.get_id(1));
        const auto x = static_cast<uint32_t>(it.get_id(2));
        const size_t out = (static_cast<size_t>(ch) * args.dst_h + y) * args.dst_w + x;
        args.dst[out] = scale_sample(src, args, ch, y, x);
    });
}

/* ------------------------------------------------------------------ */
/* Anti-alias filter at the decimated sample points                    */
/* vif_filter1d_s() (vertical then horizontal) + vif_dec16_s()          */
/* ------------------------------------------------------------------ */

struct DecimateArgs {
    float *dst;        /* channels x down_h x down_w */
    const float *taps; /* antialias taps */
    uint32_t width;    /* taps */
    uint32_t src_w;    /* scaled plane */
    uint32_t src_h;
    uint32_t down_w;
    uint32_t down_h;
};

} // namespace

namespace
{

template <class Source>
inline float antialias_at(const Source &src, const DecimateArgs &a, uint32_t ch, uint32_t i,
                          uint32_t j)
{
    const auto radius = static_cast<int32_t>(a.width / 2u);
    const auto row = static_cast<int32_t>(i * kDecimation);
    const auto col = static_cast<int32_t>(j * kDecimation);
    const bool rows_inside = window_inside(row, radius, a.src_h);
    const bool cols_inside = window_inside(col, radius, a.src_w);
    float horizontal = 0.0f;
    for (uint32_t kx = 0; kx < a.width; kx++) {
        const uint32_t c = tap(col, radius, kx, a.src_w, cols_inside);
        float vertical = 0.0f;
        for (uint32_t ky = 0; ky < a.width; ky++) {
            const uint32_t r = tap(row, radius, ky, a.src_h, rows_inside);
            const float product = a.taps[ky] * src(ch, r, c);
            vertical = vertical + product;
        }
        const float product = a.taps[kx] * vertical;
        horizontal = horizontal + product;
    }
    return horizontal;
}

template <class Source>
void launch_decimate(sycl::queue &q, const Source &src, const DecimateArgs &args, uint32_t channels)
{
    const sycl::range<3> range(channels, args.down_h, args.down_w);
    q.parallel_for(range, [=](sycl::item<3> it) {
        const auto ch = static_cast<uint32_t>(it.get_id(0));
        const auto i = static_cast<uint32_t>(it.get_id(1));
        const auto j = static_cast<uint32_t>(it.get_id(2));
        const size_t out = (static_cast<size_t>(ch) * args.down_h + i) * args.down_w + j;
        args.dst[out] = antialias_at(src, args, ch, i, j);
    });
}

/* ------------------------------------------------------------------ */
/* Local mean subtraction + independent term                           */
/* filter_and_downscale() tail + compute_independent_term()            */
/* ------------------------------------------------------------------ */

} // namespace

namespace
{

struct CentreArgs {
    const float *down; /* channels x down_h x down_w */
    const float *taps; /* lowpass taps */
    float *centered;   /* channels x trunc_h x trunc_w */
    float *indterm;    /* channels x 25 x blocks */
    uint32_t width;    /* taps */
    uint32_t down_w;
    uint32_t down_h;
    uint32_t trunc_w;
    uint32_t trunc_h;
    uint32_t blocks_h;
    uint32_t blocks;
};

inline float lowpass_at(const float *plane, const CentreArgs &a, uint32_t i, uint32_t j)
{
    const auto radius = static_cast<int32_t>(a.width / 2u);
    const auto row = static_cast<int32_t>(i);
    const auto col = static_cast<int32_t>(j);
    const bool rows_inside = window_inside(row, radius, a.down_h);
    const bool cols_inside = window_inside(col, radius, a.down_w);
    float horizontal = 0.0f;
    for (uint32_t kx = 0; kx < a.width; kx++) {
        const uint32_t c = tap(col, radius, kx, a.down_w, cols_inside);
        float vertical = 0.0f;
        for (uint32_t ky = 0; ky < a.width; ky++) {
            const uint32_t r = tap(row, radius, ky, a.down_h, rows_inside);
            const float product = a.taps[ky] * plane[static_cast<size_t>(r) * a.down_w + c];
            vertical = vertical + product;
        }
        const float product = a.taps[kx] * vertical;
        horizontal = horizontal + product;
    }
    return horizontal;
}

} // namespace

namespace
{

void launch_centre(sycl::queue &q, const CentreArgs &args, uint32_t channels)
{
    const sycl::range<3> range(channels, args.trunc_h, args.trunc_w);
    q.parallel_for(range, [=](sycl::item<3> it) {
        const auto ch = static_cast<uint32_t>(it.get_id(0));
        const auto i = static_cast<uint32_t>(it.get_id(1));
        const auto j = static_cast<uint32_t>(it.get_id(2));
        const float *plane = args.down + static_cast<size_t>(ch) * args.down_h * args.down_w;
        const float smooth = lowpass_at(plane, args, i, j);
        const float value = plane[static_cast<size_t>(i) * args.down_w + j] - smooth;
        const size_t plane_size = static_cast<size_t>(args.trunc_h) * args.trunc_w;
        args.centered[ch * plane_size + static_cast<size_t>(i) * args.trunc_w + j] = value;
        const uint32_t element = (i % kBlock) * kBlock + (j % kBlock);
        const uint32_t tile = (i / kBlock) * args.blocks_h + (j / kBlock);
        const size_t term_size = static_cast<size_t>(kN) * args.blocks;
        args.indterm[ch * term_size + static_cast<size_t>(element) * args.blocks + tile] = value;
    });
}

/* ------------------------------------------------------------------ */
/* Means: compute_mean(), one sequential fp32 sum per element          */
/* ------------------------------------------------------------------ */

struct MeansArgs {
    const float *centered;
    float *means; /* channels x 25 */
    uint32_t trunc_w;
    uint32_t trunc_h;
    uint32_t sub_w;
    uint32_t sub_h;
};

constexpr uint32_t kMeanChunk = 16u;

} // namespace

namespace
{

/* One row of the sequential sum. The chunk's loads are independent of the
 * running sum, so issuing them together hides the memory latency; the adds
 * still happen one at a time in the reference order. */
inline float add_row(float sum, const float *row, uint32_t width)
{
    uint32_t j = 0;
    for (; j + kMeanChunk <= width; j += kMeanChunk) {
        float chunk[kMeanChunk];
#pragma unroll
        for (uint32_t k = 0; k < kMeanChunk; k++) {
            chunk[k] = row[j + k];
        }
#pragma unroll
        for (const float value : chunk) {
            sum = sum + value;
        }
    }
    for (; j < width; j++) {
        sum = sum + row[j];
    }
    return sum;
}

inline float submatrix_mean(const float *plane, const MeansArgs &a, uint32_t element)
{
    const uint32_t row0 = element / kBlock;
    const uint32_t col0 = element % kBlock;
    float sum = 0.0f;
    for (uint32_t i = 0; i < a.sub_h; i++) {
        sum = add_row(sum, plane + static_cast<size_t>(row0 + i) * a.trunc_w + col0, a.sub_w);
    }
    const auto count = static_cast<float>(a.sub_w * a.sub_h);
    return div_rn(sum, count);
}

void launch_means(sycl::queue &q, const MeansArgs &args, uint32_t channels)
{
    const sycl::range<2> range(channels, kN);
    q.parallel_for(range, [=](sycl::item<2> it) {
        const auto ch = static_cast<uint32_t>(it.get_id(0));
        const auto element = static_cast<uint32_t>(it.get_id(1));
        const float *plane = args.centered + static_cast<size_t>(ch) * args.trunc_h * args.trunc_w;
        args.means[ch * kN + element] = submatrix_mean(plane, args, element);
    });
}

/* ------------------------------------------------------------------ */
/* Covariance: compute_covariance_matrix()                             */
/* The reference accumulates (x - mean_x) * (y - mean_y) in fp64. Here */
/* each difference and product is exact as an fp32 pair and the sum is */
/* carried in fp32 pairs; the quotient is rounded once to fp32.        */
/* ------------------------------------------------------------------ */

} // namespace

namespace
{

struct CovArgs {
    const float *centered;
    const float *means;
    float *cov; /* channels x 625 */
    uint32_t trunc_w;
    uint32_t trunc_h;
    uint32_t sub_w;
    uint32_t sub_h;
    uint32_t group; /* work-group size, a power of two <= kGroup */
};

inline void triangle_entry(uint32_t index, uint32_t &x, uint32_t &y)
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

inline Ff centred_product(float vx, float mx, float vy, float my)
{
    const Ff dx = two_sum(vx, -mx);
    const Ff dy = two_sum(vy, -my);
    const Ff main = two_prod(dx.hi, dy.hi);
    const float cross1 = dx.hi * dy.lo;
    const float cross2 = dx.lo * dy.hi;
    const float cross = cross1 + cross2;
    return {.hi = main.hi, .lo = main.lo + cross};
}

/* Compensated accumulation of one term: the high parts are summed exactly,
 * every low-order part lands in `lo`; normalised once per work item. */
inline Ff accumulate(Ff acc, Ff term)
{
    const Ff high = two_sum(acc.hi, term.hi);
    const float low = term.lo + high.lo;
    return {.hi = high.hi, .lo = acc.lo + low};
}

} // namespace

namespace
{

inline Ff covariance_partial(const float *plane, const CovArgs &a, uint32_t x, uint32_t y, float mx,
                             float my, uint32_t lid)
{
    const uint32_t xr = x / kBlock;
    const uint32_t xc = x % kBlock;
    const uint32_t yr = y / kBlock;
    const uint32_t yc = y % kBlock;
    const uint32_t total = a.sub_w * a.sub_h;
    Ff acc{.hi = 0.0f, .lo = 0.0f};
    for (uint32_t pos = lid; pos < total; pos += a.group) {
        const uint32_t row = pos / a.sub_w;
        const uint32_t col = pos % a.sub_w;
        const float vx = plane[static_cast<size_t>(xr + row) * a.trunc_w + xc + col];
        const float vy = plane[static_cast<size_t>(yr + row) * a.trunc_w + yc + col];
        acc = accumulate(acc, centred_product(vx, mx, vy, my));
    }
    return quick_two_sum(acc.hi, acc.lo);
}

inline void covariance_group(sycl::nd_item<1> it, const CovArgs &a, float *hi, float *lo)
{
    const auto group = static_cast<uint32_t>(it.get_group(0));
    const uint32_t ch = group / kTriangle;
    uint32_t x = 0u;
    uint32_t y = 0u;
    triangle_entry(group % kTriangle, x, y);
    const auto lid = static_cast<uint32_t>(it.get_local_id(0));
    const float *plane = a.centered + static_cast<size_t>(ch) * a.trunc_h * a.trunc_w;
    const float mx = a.means[ch * kN + x];
    const float my = a.means[ch * kN + y];
    const Ff part = covariance_partial(plane, a, x, y, mx, my, lid);
    hi[lid] = part.hi;
    lo[lid] = part.lo;
    sycl::group_barrier(it.get_group());
    for (uint32_t span = a.group / 2u; span > 0u; span >>= 1u) {
        if (lid < span) {
            const Ff merged = ff_add({.hi = hi[lid], .lo = lo[lid]},
                                     {.hi = hi[lid + span], .lo = lo[lid + span]});
            hi[lid] = merged.hi;
            lo[lid] = merged.lo;
        }
        sycl::group_barrier(it.get_group());
    }
    if (lid == 0u) {
        const auto count = static_cast<float>(a.sub_w * a.sub_h);
        const float value = ff_div_to_float({.hi = hi[0], .lo = lo[0]}, count);
        float *matrix = a.cov + static_cast<size_t>(ch) * kMatrix;
        matrix[x * kN + y] = value;
        matrix[y * kN + x] = value;
    }
}

} // namespace

namespace
{

void launch_covariance(sycl::queue &q, const CovArgs &args, uint32_t channels)
{
    const size_t groups = static_cast<size_t>(channels) * kTriangle;
    q.submit([&](sycl::handler &h) {
        const sycl::local_accessor<float, 1> hi(sycl::range<1>(args.group), h);
        const sycl::local_accessor<float, 1> lo(sycl::range<1>(args.group), h);
        h.parallel_for(
            sycl::nd_range<1>(groups * args.group, args.group), [=](sycl::nd_item<1> it) {
                covariance_group(it, args, hi.get_multi_ptr<sycl::access::decorated::no>().get(),
                                 lo.get_multi_ptr<sycl::access::decorated::no>().get());
            });
    });
}

/* Enough work items per covariance entry to cover the submatrix, no more:
 * a 256-wide group for an 11x6 submatrix is 190 idle items and a deeper
 * reduction tree. */
uint32_t covariance_group_size(uint32_t terms)
{
    uint32_t group = 32u;
    while (group < kGroup && group < terms / 8u) {
        group *= 2u;
    }
    return group;
}

/* ------------------------------------------------------------------ */
/* Eigenvalues: compute_eigenvalues() (Householder tridiagonalisation  */
/* + implicit-shift QR), regularity, matrix_qr_decomposition()         */
/* ------------------------------------------------------------------ */

struct LinalgArgs {
    const float *cov;
    float *eig;      /* channels x 25 */
    float *qmat;     /* channels x 625, accumulated reflector product */
    float *rmat;     /* channels x 625 */
    int32_t *status; /* channels x 2: singular, iteration cap */
};

constexpr size_t kSlmStride = 640u;
constexpr size_t kSlmVector = 32u;
constexpr size_t kSlmFloats = 6u * kSlmStride + 5u * kSlmVector;

} // namespace

namespace
{

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

inline Slm slm_layout(float *base)
{
    float *vectors = base + 6u * kSlmStride;
    return {.a = base,
            .cov = base + kSlmStride,
            .z = base + 2u * kSlmStride,
            .q = base + 3u * kSlmStride,
            .h = base + 4u * kSlmStride,
            .t = base + 5u * kSlmStride,
            .vec = vectors,
            .x = vectors + kSlmVector,
            .d = vectors + 2u * kSlmVector,
            .sd = vectors + 3u * kSlmVector,
            .scalar = vectors + 4u * kSlmVector};
}

inline void group_sync(sycl::nd_item<1> it)
{
    sycl::group_barrier(it.get_group());
}

inline float sign_of(float x)
{
    return x >= 0.0f ? 1.0f : -1.0f;
}

/* pythagoras(), speed.c. */
inline float pythagoras(float x, float y)
{
    const float xx = x * x;
    const float yy = y * y;
    return sqrt_rn(xx + yy);
}

} // namespace

namespace
{

/* compute_column_norm(), speed.c. */
inline float column_norm(const float *a, uint32_t col, uint32_t start)
{
    float norm = 0.0f;
    for (uint32_t r = start; r < kN; r++) {
        const float value = a[r * kN + col];
        const float square = value * value;
        norm = norm + square;
    }
    return sqrt_rn(norm);
}

/* The scalar half of compute_householder_transform(), speed.c, on one work
 * item: stores tau, the column divisor s (0 when the column is left alone)
 * and beta. The column division itself is element-wise, so the group does it
 * (householder_scale). */
inline void householder_scalars(const Slm &m, uint32_t col, uint32_t start)
{
    m.scalar[0] = 0.0f;
    m.scalar[5] = 0.0f;
    if (kN - start == 1u) {
        return;
    }
    const float xnorm = column_norm(m.a, col, start + 1u);
    if (xnorm == 0.0f) {
        return;
    }
    const float alpha = m.a[start * kN + col];
    const float beta = -sign_of(alpha) * pythagoras(alpha, xnorm);
    m.scalar[0] = div_rn(beta - alpha, beta);
    m.scalar[5] = alpha - beta;
    m.scalar[6] = beta;
}

/* `A[i][col] /= s` for the column, then `A[start][col] = beta`. */
inline void householder_scale(sycl::nd_item<1> it, const Slm &m, uint32_t col, uint32_t start)
{
    const auto lid = static_cast<uint32_t>(it.get_local_id(0));
    const float s = m.scalar[5];
    if (s == 0.0f) {
        return;
    }
    for (uint32_t r = start + lid; r < kN; r += kLinalgGroup) {
        m.a[r * kN + col] = div_rn(m.a[r * kN + col], s);
    }
    group_sync(it);
    if (lid == 0u) {
        m.a[start * kN + col] = m.scalar[6];
    }
    group_sync(it);
}

} // namespace

namespace
{

/* x = tau * A * v, then x += alpha * v (tridiagonal_multiply/_axpy). */
inline void tridiagonal_vectors(sycl::nd_item<1> it, const Slm &m, uint32_t start, float tau)
{
    const auto lid = static_cast<uint32_t>(it.get_local_id(0));
    for (uint32_t r = start + lid; r < kN; r += kLinalgGroup) {
        float acc = 0.0f;
        for (uint32_t c = start; c < kN; c++) {
            const float scaled = tau * m.a[r * kN + c];
            const float product = scaled * m.vec[c];
            acc = acc + product;
        }
        m.x[r] = acc;
    }
    group_sync(it);
    if (lid == 0u) {
        float xv = 0.0f;
        for (uint32_t r = start; r < kN; r++) {
            const float product = m.x[r] * m.vec[r];
            xv = xv + product;
        }
        const float half_tau = -0.5f * tau;
        m.scalar[1] = half_tau * xv;
    }
    group_sync(it);
    const float alpha = m.scalar[1];
    for (uint32_t r = start + lid; r < kN; r += kLinalgGroup) {
        const float product = alpha * m.vec[r];
        m.x[r] = m.x[r] + product;
    }
    group_sync(it);
}

} // namespace

namespace
{

/* One step of convert_to_tridiagonal(), speed.c. */
inline void tridiagonal_step(sycl::nd_item<1> it, const Slm &m, uint32_t i)
{
    const auto lid = static_cast<uint32_t>(it.get_local_id(0));
    const uint32_t start = i + 1u;
    if (lid == 0u) {
        householder_scalars(m, i, start);
    }
    group_sync(it);
    householder_scale(it, m, i, start);
    const float tau = m.scalar[0];
    if (lid == 0u) {
        for (uint32_t j = start; j < kN; j++) {
            m.vec[j] = m.a[j * kN + i];
        }
        if (tau != 0.0f) {
            m.a[start * kN + i] = m.vec[start];
            m.vec[start] = 1.0f;
        }
    }
    group_sync(it);
    if (tau == 0.0f) {
        return;
    }
    tridiagonal_vectors(it, m, start, tau);
    const uint32_t span = kN - start;
    for (uint32_t idx = lid; idx < span * span; idx += kLinalgGroup) {
        const uint32_t r = start + idx / span;
        const uint32_t c = start + idx % span;
        const float p1 = m.x[r] * m.vec[c];
        const float p2 = m.vec[r] * m.x[c];
        const float both = p1 + p2;
        m.a[r * kN + c] = m.a[r * kN + c] - both;
    }
    group_sync(it);
}

/* chop_small_elements(), speed.c. */
inline void chop_small(float *d, float *sd, uint32_t n)
{
    for (uint32_t i = 0; i + 1u < n; i++) {
        const float bound = sycl::fabs(d[i]) + sycl::fabs(d[i + 1u]);
        if (below_eps_scaled(sycl::fabs(sd[i]), bound)) {
            sd[i] = 0.0f;
        }
    }
}

} // namespace

namespace
{

/* trailing_eigenvalue(), speed.c. */
inline float trailing_eigenvalue(const float *d, const float *sd, uint32_t n)
{
    const float ta = d[n - 2u];
    const float tb = d[n - 1u];
    const float tab = sd[n - 2u];
    const float dt = div_rn(ta - tb, 2.0f);
    if (dt > 0.0f) {
        const float den = dt + pythagoras(dt, tab);
        const float ratio = div_rn(tab, den);
        const float step = tab * ratio;
        return tb - step;
    }
    if (dt == 0.0f) {
        return tb - sycl::fabs(tab);
    }
    const float den = -dt + pythagoras(dt, tab);
    const float ratio = div_rn(tab, den);
    const float step = tab * ratio;
    return tb + step;
}

/* create_givens(), speed.c. */
inline void create_givens(float a, float b, float &c, float &s)
{
    if (b == 0.0f) {
        c = 1.0f;
        s = 0.0f;
        return;
    }
    const bool b_larger = sycl::fabs(b) > sycl::fabs(a);
    const float t = b_larger ? div_rn(-a, b) : div_rn(-b, a);
    const float tt = t * t;
    const float root = sqrt_rn(1.0f + tt);
    const float unit = div_rn(1.0f, root);
    const float other = unit * t;
    s = b_larger ? unit : other;
    c = b_larger ? other : unit;
}

struct Rotated {
    float ak;
    float bk;
    float ap;
};

} // namespace

namespace
{

/* G' T G of qr_step_size2() / qr_step_general(), speed.c. */
inline Rotated rotate(float c, float s, float ap, float bp, float aq)
{
    const float cap = c * ap;
    const float sbp = s * bp;
    const float saq = s * aq;
    const float cbp = c * bp;
    const float sap = s * ap;
    const float caq = c * aq;
    const float l1 = c * (cap - sbp);
    const float l2 = s * (saq - cbp);
    const float m1 = c * (sap + cbp);
    const float m2 = s * (sbp + caq);
    const float n1 = s * (sap + cbp);
    const float n2 = c * (sbp + caq);
    return {.ak = l1 + l2, .bk = m1 - m2, .ap = n1 + n2};
}

inline void qr_step_size2(float *d, float *sd, float x, float z)
{
    float c = 0.0f;
    float s = 0.0f;
    create_givens(x, z, c, s);
    const Rotated r = rotate(c, s, d[0], sd[0], d[1]);
    d[0] = r.ak;
    sd[0] = r.bk;
    d[1] = r.ap;
}

} // namespace

namespace
{

inline void qr_step_general(float *d, float *sd, uint32_t n, float x, float z)
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
        const float cbk = c * bk;
        const float szk = s * zk;
        const float bk1 = cbk - szk;
        const Rotated r = rotate(c, s, ap, bp, aq);
        const float zp1 = -s * bq;
        const float bq1 = c * bq;
        bk = r.bk;
        zk = zp1;
        ap = r.ap;
        bp = bq1;
        if (k + 2u < n) {
            aq = d[k + 2u];
        }
        if (k + 3u < n) {
            bq = sd[k + 2u];
        }
        d[k] = r.ak;
        if (k > 0u) {
            sd[k - 1u] = bk1;
        }
        if (k + 2u < n) {
            sd[k + 1u] = bp;
        }
        x = bk;
        z = zk;
    }
    d[n - 1u] = ap;
    sd[n - 2u] = bk;
}

} // namespace

namespace
{

/* qr_step(), speed.c. */
inline void qr_step(float *d, float *sd, uint32_t n)
{
    float mu = trailing_eigenvalue(d, sd, n);
    const float scale = sycl::fabs(d[0]) + sycl::fabs(sd[0]);
    if (below_eps_scaled(scale, sycl::fabs(mu))) {
        mu = 0.0f;
    }
    const float x = d[0] - mu;
    const float z = sd[0];
    if (n == 2u) {
        qr_step_size2(d, sd, x, z);
        return;
    }
    qr_step_general(d, sd, n, x, z);
}

/* compute_eigenvalues_tridiagonal(), speed.c. Returns whether the iteration
 * cap was reached. */
inline bool eigenvalues_tridiagonal(float *d, float *sd)
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
        for (uint32_t probe = 0; probe < kN && a > 0u && sd[a - 1u] != 0.0f; probe++) {
            a--;
        }
        const uint32_t n_block = b - a + 1u;
        qr_step(d + a, sd + a, n_block);
        chop_small(d + a, sd + a, n_block);
        iter++;
    }
    return iter == kQrIterationCap;
}

} // namespace

namespace
{

/* Tridiagonal QR on work-item 0; eigenvalues, regularity and the cap flag. */
inline void eigen_serial(const Slm &m, float *eig)
{
    for (uint32_t r = 0; r < kN; r++) {
        m.d[r] = m.a[r * kN + r];
    }
    for (uint32_t r = 0; r + 1u < kN; r++) {
        m.sd[r] = m.a[(r + 1u) * kN + r];
    }
    const bool capped = eigenvalues_tridiagonal(m.d, m.sd);
    bool regular = true;
    for (uint32_t r = 0; r < kN; r++) {
        eig[r] = m.d[r];
        if (below_eps(m.d[r])) { /* is_matrix_regular() */
            regular = false;
        }
    }
    m.scalar[3] = regular ? 1.0f : 0.0f;
    m.scalar[4] = capped ? 1.0f : 0.0f;
}

/* matrix_mul() with speed_matmul_scalar() accumulation order. */
inline void group_matmul(sycl::nd_item<1> it, float *dst, const float *x, const float *y)
{
    for (auto idx = static_cast<uint32_t>(it.get_local_id(0)); idx < kMatrix; idx += kLinalgGroup) {
        const uint32_t i = idx / kN;
        const uint32_t j = idx % kN;
        float acc = 0.0f;
        for (uint32_t k = 0; k < kN; k++) {
            const float product = x[i * kN + k] * y[k * kN + j];
            acc = acc + product;
        }
        dst[idx] = acc;
    }
    group_sync(it);
}

/* vector_norm(), speed.c. */
inline float vector_norm(const float *v)
{
    float sum = 0.0f;
    for (uint32_t i = 0; i < kN; i++) {
        const float square = v[i] * v[i];
        sum = sum + square;
    }
    return sqrt_rn(sum);
}

} // namespace

namespace
{

/* matrix_minor() of the working matrix, then its k-th column into m.vec. */
inline void qr_minor_column(sycl::nd_item<1> it, const Slm &m, uint32_t k)
{
    const auto lid = static_cast<uint32_t>(it.get_local_id(0));
    for (uint32_t idx = lid; idx < kMatrix; idx += kLinalgGroup) {
        const uint32_t i = idx / kN;
        const uint32_t j = idx % kN;
        if (i < k || j < k) {
            m.z[idx] = i == j ? 1.0f : 0.0f;
        }
    }
    group_sync(it);
    for (uint32_t r = lid; r < kN; r += kLinalgGroup) {
        m.vec[r] = m.z[r * kN + k];
    }
    group_sync(it);
}

/* One column of matrix_qr_decomposition(): minor, reflector vector and
 * I - 2 v v^T. Returns false when the reflector vanishes. */
inline bool qr_reflector(sycl::nd_item<1> it, const Slm &m, uint32_t k)
{
    const auto lid = static_cast<uint32_t>(it.get_local_id(0));
    qr_minor_column(it, m, k);
    if (lid == 0u) {
        const float norm = vector_norm(m.vec);
        const float shift = sign_of(m.cov[k * kN + k]) * norm;
        m.vec[k] = m.vec[k] + shift;
        m.scalar[2] = vector_norm(m.vec);
    }
    group_sync(it);
    const float vn = m.scalar[2];
    if (vn == 0.0f) {
        return false;
    }
    for (uint32_t r = lid; r < kN; r += kLinalgGroup) {
        m.vec[r] = div_rn(m.vec[r], vn);
    }
    group_sync(it);
    for (uint32_t idx = lid; idx < kMatrix; idx += kLinalgGroup) {
        const uint32_t i = idx / kN;
        const uint32_t j = idx % kN;
        const float scaled = -2.0f * m.vec[i];
        const float value = scaled * m.vec[j];
        m.h[idx] = i == j ? value + 1.0f : value;
    }
    group_sync(it);
    return true;
}

} // namespace

namespace
{

/* matrix_qr_decomposition(), speed.c: leaves the accumulated reflector product
 * (the matrix solve_linear_system() multiplies B by) in m.q and R in m.t. */
inline void qr_decompose(sycl::nd_item<1> it, Slm &m)
{
    const auto lid = static_cast<uint32_t>(it.get_local_id(0));
    for (uint32_t idx = lid; idx < kMatrix; idx += kLinalgGroup) {
        m.z[idx] = m.cov[idx];
        m.q[idx] = (idx / kN == idx % kN) ? 1.0f : 0.0f;
    }
    group_sync(it);
    for (uint32_t k = 0; k + 1u < kN; k++) {
        if (!qr_reflector(it, m, k)) {
            continue;
        }
        group_matmul(it, m.t, m.h, m.z);
        std::swap(m.z, m.t);
        group_matmul(it, m.t, m.h, m.q);
        std::swap(m.q, m.t);
    }
    group_matmul(it, m.t, m.q, m.cov);
}

/* solve_triangular_system()'s pivot rejection, speed.c. */
inline bool pivot_singular(const float *r)
{
    bool singular = false;
    for (uint32_t i = 0; i < kN; i++) {
        if (below_eps(sycl::fabs(r[i * kN + i]))) {
            singular = true;
        }
    }
    return singular;
}

inline void linalg_store(sycl::nd_item<1> it, const LinalgArgs &a, const Slm &m, uint32_t ch,
                         bool regular)
{
    const auto lid = static_cast<uint32_t>(it.get_local_id(0));
    if (regular) {
        for (uint32_t idx = lid; idx < kMatrix; idx += kLinalgGroup) {
            a.qmat[ch * kMatrix + idx] = m.q[idx];
            a.rmat[ch * kMatrix + idx] = m.t[idx];
        }
    }
    if (lid == 0u) {
        const bool singular = !regular || pivot_singular(m.t);
        const size_t slot = static_cast<size_t>(ch) * 2u;
        a.status[slot] = singular ? 1 : 0;
        a.status[slot + 1u] = m.scalar[4] != 0.0f ? 1 : 0;
    }
}

} // namespace

namespace
{

inline void linalg_group(sycl::nd_item<1> it, const LinalgArgs &a, float *base)
{
    Slm m = slm_layout(base);
    const auto ch = static_cast<uint32_t>(it.get_group(0));
    const auto lid = static_cast<uint32_t>(it.get_local_id(0));
    for (uint32_t idx = lid; idx < kMatrix; idx += kLinalgGroup) {
        const float value = a.cov[ch * kMatrix + idx];
        m.a[idx] = value;
        m.cov[idx] = value;
    }
    group_sync(it);
    for (uint32_t i = 0; i + 2u < kN; i++) {
        tridiagonal_step(it, m, i);
    }
    if (lid == 0u) {
        eigen_serial(m, a.eig + static_cast<size_t>(ch) * kN);
    }
    group_sync(it);
    const bool regular = m.scalar[3] != 0.0f;
    if (regular) {
        qr_decompose(it, m);
    }
    linalg_store(it, a, m, ch, regular);
}

void launch_linalg(sycl::queue &q, const LinalgArgs &args, uint32_t channels)
{
    q.submit([&](sycl::handler &h) {
        const sycl::local_accessor<float, 1> slm(sycl::range<1>(kSlmFloats), h);
        h.parallel_for(
            sycl::nd_range<1>(static_cast<size_t>(channels) * kLinalgGroup, kLinalgGroup),
            [=](sycl::nd_item<1> it) {
                linalg_group(it, args, slm.get_multi_ptr<sycl::access::decorated::no>().get());
            });
    });
}

/* ------------------------------------------------------------------ */
/* Solve, variance and entropy per block: solve_linear_system() tail,  */
/* compute_pointwise_product_and_division(), sum_columns(),            */
/* update_entropy()                                                    */
/* ------------------------------------------------------------------ */

} // namespace

namespace
{

struct SolveArgs {
    const float *indterm;
    const float *qmat;
    const float *rmat;
    const float *eig;
    const int32_t *status;
    float *var; /* channels x blocks */
    float *ent;
    uint32_t blocks;
    float sigma_nn;
    float entropy_constant;
};

inline void solve_block(const float *b, uint32_t stride, const float *q, const float *r,
                        float *solution)
{
    float y[kN];
    for (uint32_t i = 0; i < kN; i++) {
        float acc = 0.0f;
        for (uint32_t k = 0; k < kN; k++) {
            const float product = q[i * kN + k] * b[static_cast<size_t>(k) * stride];
            acc = acc + product;
        }
        y[i] = acc;
    }
    for (uint32_t step = 0; step < kN; step++) {
        const uint32_t i = kN - 1u - step;
        float term = y[i];
        for (uint32_t k = i + 1u; k < kN; k++) {
            const float product = solution[k] * r[i * kN + k];
            term = term - product;
        }
        solution[i] = div_rn(term, r[i * kN + i]);
    }
}

} // namespace

namespace
{

inline void block_statistics(const SolveArgs &a, uint32_t ch, uint32_t block)
{
    const float *b = a.indterm + static_cast<size_t>(ch) * kN * a.blocks + block;
    float solution[kN];
    for (float &value : solution) {
        value = 0.0f;
    }
    if (a.status[static_cast<size_t>(ch) * 2u] == 0) {
        const size_t matrix = static_cast<size_t>(ch) * kMatrix;
        solve_block(b, a.blocks, a.qmat + matrix, a.rmat + matrix, solution);
    }
    const float first = solution[0] * b[0];
    float variance = div_rn(first, kElementsF);
    for (uint32_t e = 1; e < kN; e++) {
        const float product = solution[e] * b[static_cast<size_t>(e) * a.blocks];
        const float term = div_rn(product, kElementsF);
        variance = variance + term;
    }
    float entropy = 0.0f;
    for (uint32_t k = 0; k < kN; k++) {
        const float eigenvalue = a.eig[ch * kN + k];
        const float l = eigenvalue < 0.0f ? 0.0f : eigenvalue;
        const float scaled = l * variance;
        const float shifted = scaled + a.sigma_nn;
        const float term = speed_log2(shifted) + a.entropy_constant;
        entropy = entropy + term;
    }
    a.var[static_cast<size_t>(ch) * a.blocks + block] = variance;
    a.ent[static_cast<size_t>(ch) * a.blocks + block] = entropy;
}

void launch_solve(sycl::queue &q, const SolveArgs &args, uint32_t channels)
{
    q.parallel_for(sycl::range<2>(channels, args.blocks), [=](sycl::item<2> it) {
        block_statistics(args, static_cast<uint32_t>(it.get_id(0)),
                         static_cast<uint32_t>(it.get_id(1)));
    });
}

/* ------------------------------------------------------------------ */
/* Frame score: get_speed_score() + speed_extract_score()              */
/* ------------------------------------------------------------------ */

} // namespace

namespace
{

struct ScoreArgs {
    const float *var;
    const float *ent;
    const int32_t *status;
    float *contrib; /* pairs x blocks */
    FrameResult *result;
    uint32_t blocks;
    float base_entropy;
    int32_t weight_mode;
};

inline float weighted_log(float variance)
{
    return speed_log2(1.0f + variance);
}

inline float spatial_dis_weight(float rv, float dv, int32_t mode)
{
    if (mode == 0 || mode == 2) {
        return weighted_log(dv);
    }
    if (mode == 1) {
        return weighted_log(rv);
    }
    if (mode == 5 || mode == 6) {
        const float ref_share = mode == 5 ? 0.75f : 0.25f;
        const float dis_share = mode == 5 ? 0.25f : 0.75f;
        const float wr = ref_share * rv;
        const float wd = dis_share * dv;
        return weighted_log(wr + wd);
    }
    const float mean = div_rn(rv + dv, 2.0f); /* modes 3 and 4 */
    return weighted_log(mean);
}

inline float spatial_ref_weight(float rv, float dv, int32_t mode)
{
    if (mode == 2) {
        return weighted_log(dv);
    }
    if (mode == 3) {
        const float mean = div_rn(rv + dv, 2.0f);
        return weighted_log(mean);
    }
    return weighted_log(rv);
}

} // namespace

namespace
{

inline float block_score(float re, float de, float rv, float dv, const ScoreArgs &a)
{
    if (re < a.base_entropy && de < a.base_entropy) {
        return 0.0f;
    }
    const float spatial_ref = re * spatial_ref_weight(rv, dv, a.weight_mode);
    const float spatial_dis = de * spatial_dis_weight(rv, dv, a.weight_mode);
    return sycl::fabs(spatial_ref - spatial_dis);
}

inline void score_group(sycl::nd_item<1> it, const ScoreArgs &a)
{
    const auto pair = static_cast<uint32_t>(it.get_group(0));
    const auto lid = static_cast<uint32_t>(it.get_local_id(0));
    const size_t ref = static_cast<size_t>(2u * pair) * a.blocks;
    const size_t dis = ref + a.blocks;
    float *contrib = a.contrib + static_cast<size_t>(pair) * a.blocks;
    for (uint32_t b = lid; b < a.blocks; b += kGroup) {
        contrib[b] = block_score(a.ent[ref + b], a.ent[dis + b], a.var[ref + b], a.var[dis + b], a);
    }
    sycl::group_barrier(it.get_group(), sycl::memory_scope::work_group);
    if (lid != 0u) {
        return;
    }
    float score = 0.0f;
    for (uint32_t b = 0; b < a.blocks; b++) {
        score = score + contrib[b];
    }
    score = div_rn(score, static_cast<float>(a.blocks));
    const size_t slots = static_cast<size_t>(pair) * 4u;
    const int32_t sing_ref = a.status[slots];
    const int32_t sing_dis = a.status[slots + 2u];
    if ((sing_ref != 0) != (sing_dis != 0)) {
        score = 0.0f;
    }
    a.result->score[pair] = score;
    for (uint32_t side = 0; side < 2u; side++) {
        const uint32_t ch = 2u * pair + side;
        const size_t slot = static_cast<size_t>(ch) * 2u;
        a.result->singular[ch] = a.status[slot];
        a.result->iteration_cap[ch] = a.status[slot + 1u];
    }
}

void launch_score(sycl::queue &q, const ScoreArgs &args, uint32_t pairs)
{
    q.parallel_for(sycl::nd_range<1>(static_cast<size_t>(pairs) * kGroup, kGroup),
                   [=](sycl::nd_item<1> it) { score_group(it, args); });
}

} // namespace

/* ------------------------------------------------------------------ */
/* Host orchestration                                                  */
/* ------------------------------------------------------------------ */

namespace
{

constexpr uint32_t kMaxGraphs = 2u;
#if defined(SYCL_EXT_ONEAPI_GRAPH)
namespace syclex = sycl::ext::oneapi::experimental;
using ExecGraph = syclex::command_graph<syclex::graph_state::executable>;
#endif

} // namespace

struct speed_sycl::Pipeline {
    sycl::queue *queue;
    PipelineConfig config;
    size_t plane_bytes;
    unsigned char *staging;
    unsigned char *raw;
    float *taps; /* antialias[kMaxTaps] then lowpass[kMaxTaps] */
    float *scaled;
    float *down;
    float *centered;
    float *indterm;
    float *means;
    float *cov;
    float *eig;
    float *qmat;
    float *rmat;
    float *var;
    float *ent;
    float *contrib;
    int32_t *status;
    FrameResult *result_device;
    FrameResult *result_host;
    /* Recorded per-frame chains, one per distinct channel binding (chroma
     * needs one, temporal two: the slots alternate). */
    uint32_t graph_count;
    bool graphs_disabled;
    ChannelBinding graph_bindings[kMaxGraphs][kMaxChannels];
#if defined(SYCL_EXT_ONEAPI_GRAPH)
    ExecGraph *graphs[kMaxGraphs];
#endif
};

namespace
{

template <typename T> T *device_alloc(sycl::queue &q, size_t count)
{
    return count == 0u ? nullptr : sycl::malloc_device<T>(count, q);
}

void allocate_planes(Pipeline &p)
{
    sycl::queue &q = *p.queue;
    const Geometry &g = p.config.geometry;
    const size_t ch = p.config.channels;
    p.staging = sycl::malloc_host<unsigned char>(p.plane_bytes * p.config.staged, q);
    p.raw = device_alloc<unsigned char>(q, p.plane_bytes * p.config.raw_planes);
    p.taps = device_alloc<float>(q, size_t{2} * kMaxTaps);
    if (g.prescale != 0) {
        p.scaled = device_alloc<float>(q, ch * g.scaled_w * g.scaled_h);
    }
    p.down = device_alloc<float>(q, ch * g.down_w * g.down_h);
    p.centered = device_alloc<float>(q, ch * g.trunc_w * g.trunc_h);
    p.indterm = device_alloc<float>(q, ch * kN * g.blocks);
}

void allocate_linalg(Pipeline &p)
{
    sycl::queue &q = *p.queue;
    const size_t ch = p.config.channels;
    const size_t blocks = p.config.geometry.blocks;
    p.means = device_alloc<float>(q, ch * kN);
    p.cov = device_alloc<float>(q, ch * kMatrix);
    p.eig = device_alloc<float>(q, ch * kN);
    p.qmat = device_alloc<float>(q, ch * kMatrix);
    p.rmat = device_alloc<float>(q, ch * kMatrix);
    p.var = device_alloc<float>(q, ch * blocks);
    p.ent = device_alloc<float>(q, ch * blocks);
    p.contrib = device_alloc<float>(q, (ch / 2u) * blocks);
    p.status = device_alloc<int32_t>(q, ch * 2u);
    p.result_device = device_alloc<FrameResult>(q, 1u);
    p.result_host = sycl::malloc_host<FrameResult>(1u, q);
}

} // namespace

namespace
{

bool allocations_complete(const Pipeline &p)
{
    const void *const required[] = {
        p.staging, p.raw,     p.taps,          p.down,   p.centered,    p.indterm,
        p.means,   p.cov,     p.eig,           p.qmat,   p.rmat,        p.var,
        p.ent,     p.contrib, p.result_device, p.status, p.result_host,
    };
    for (const void *pointer : required) {
        if (pointer == nullptr) {
            return false;
        }
    }
    return p.config.geometry.prescale == 0 || p.scaled != nullptr;
}

template <typename T> void release(sycl::queue &q, T *&pointer)
{
    if (pointer) {
        sycl::free(pointer, q);
        pointer = nullptr;
    }
}

void release_all(Pipeline &p)
{
    assert(p.queue != nullptr);
    assert(p.graph_count <= kMaxGraphs);
    sycl::queue &q = *p.queue;
    release(q, p.staging);
    release(q, p.raw);
    release(q, p.taps);
    release(q, p.scaled);
    release(q, p.down);
    release(q, p.centered);
    release(q, p.indterm);
    release(q, p.means);
    release(q, p.cov);
    release(q, p.eig);
    release(q, p.qmat);
    release(q, p.rmat);
    release(q, p.var);
    release(q, p.ent);
    release(q, p.contrib);
    release(q, p.status);
    release(q, p.result_device);
    release(q, p.result_host);
#if defined(SYCL_EXT_ONEAPI_GRAPH)
    for (uint32_t i = 0; i < p.graph_count; i++) {
        delete p.graphs[i];
        p.graphs[i] = nullptr;
    }
#endif
    p.graph_count = 0u;
}

} // namespace

namespace
{

bool in_range(uint32_t value, uint32_t low, uint32_t high)
{
    return value >= low && value <= high;
}

bool shape_valid(const PipelineConfig &c)
{
    const Geometry &g = c.geometry;
    const bool dims = g.blocks > 0u && g.sub_w > 0u && g.sub_h > 0u;
    const bool nested = g.down_w >= g.trunc_w && g.down_h >= g.trunc_h;
    return dims && nested;
}

bool config_valid(const PipelineConfig &c)
{
    const bool channels = c.channels == 2u || c.channels == 4u;
    const bool planes =
        in_range(c.raw_planes, 1u, kMaxRawPlanes) && in_range(c.staged, 1u, c.raw_planes);
    const bool taps = in_range(c.filters.antialias_width, 1u, kMaxTaps) &&
                      in_range(c.filters.lowpass_width, 1u, kMaxTaps);
    const bool mode = c.scoring.weight_mode >= 0 && c.scoring.weight_mode <= 6;
    return c.queue != nullptr && channels && planes && taps && mode && shape_valid(c);
}

void upload_taps(Pipeline &p)
{
    float host[2u * kMaxTaps];
    std::memcpy(host, p.config.filters.antialias, sizeof(float) * kMaxTaps);
    std::memcpy(host + kMaxTaps, p.config.filters.lowpass, sizeof(float) * kMaxTaps);
    p.queue->memcpy(p.taps, host, sizeof(host)).wait(); /* init only */
}

RawPlanes bind_planes(const Pipeline &p, const ChannelBinding *bindings)
{
    RawPlanes planes{};
    for (uint32_t ch = 0; ch < p.config.channels; ch++) {
        planes.minuend[ch] = p.raw + static_cast<size_t>(bindings[ch].minuend) * p.plane_bytes;
        planes.subtrahend[ch] =
            bindings[ch].subtrahend < 0 ?
                nullptr :
                p.raw + static_cast<size_t>(bindings[ch].subtrahend) * p.plane_bytes;
    }
    planes.width = p.config.geometry.src_w;
    planes.scale = p.config.geometry.sample_scale;
    return planes;
}

} // namespace

namespace
{

DecimateArgs decimate_args(const Pipeline &p)
{
    const Geometry &g = p.config.geometry;
    return {.dst = p.down,
            .taps = p.taps,
            .width = p.config.filters.antialias_width,
            .src_w = g.scaled_w,
            .src_h = g.scaled_h,
            .down_w = g.down_w,
            .down_h = g.down_h};
}

template <typename T> void enqueue_filter(Pipeline &p, const RawPlanes &planes)
{
    sycl::queue &q = *p.queue;
    const Geometry &g = p.config.geometry;
    const RawSource<T> raw(planes);
    if (g.prescale == 0) {
        launch_decimate(q, raw, decimate_args(p), p.config.channels);
        return;
    }
    const ScaleArgs scale{.dst = p.scaled,
                          .src_w = g.src_w,
                          .src_h = g.src_h,
                          .dst_w = g.scaled_w,
                          .dst_h = g.scaled_h,
                          .method = g.scale_method};
    launch_scale(q, raw, scale, p.config.channels);
    const FloatSource scaled(p.scaled, g.scaled_w, g.scaled_h);
    launch_decimate(q, scaled, decimate_args(p), p.config.channels);
}

} // namespace

namespace
{

void enqueue_statistics(Pipeline &p)
{
    sycl::queue &q = *p.queue;
    const Geometry &g = p.config.geometry;
    const uint32_t ch = p.config.channels;
    /* The 25 submatrices are the truncated plane shifted by 0..4 in each axis:
     * the mean and covariance windows end exactly at its last row and column. */
    assert(g.sub_w + kBlock - 1u == g.trunc_w && g.sub_h + kBlock - 1u == g.trunc_h);
    assert(g.trunc_w <= g.down_w && g.trunc_h <= g.down_h);
    assert(g.blocks_h * kBlock == g.trunc_w && g.blocks > 0u);
    const CentreArgs centre{.down = p.down,
                            .taps = p.taps + kMaxTaps,
                            .centered = p.centered,
                            .indterm = p.indterm,
                            .width = p.config.filters.lowpass_width,
                            .down_w = g.down_w,
                            .down_h = g.down_h,
                            .trunc_w = g.trunc_w,
                            .trunc_h = g.trunc_h,
                            .blocks_h = g.blocks_h,
                            .blocks = g.blocks};
    launch_centre(q, centre, ch);
    const MeansArgs means{.centered = p.centered,
                          .means = p.means,
                          .trunc_w = g.trunc_w,
                          .trunc_h = g.trunc_h,
                          .sub_w = g.sub_w,
                          .sub_h = g.sub_h};
    launch_means(q, means, ch);
    const CovArgs cov{.centered = p.centered,
                      .means = p.means,
                      .cov = p.cov,
                      .trunc_w = g.trunc_w,
                      .trunc_h = g.trunc_h,
                      .sub_w = g.sub_w,
                      .sub_h = g.sub_h,
                      .group = covariance_group_size(g.sub_w * g.sub_h)};
    launch_covariance(q, cov, ch);
    const LinalgArgs linalg{
        .cov = p.cov, .eig = p.eig, .qmat = p.qmat, .rmat = p.rmat, .status = p.status};
    launch_linalg(q, linalg, ch);
}

} // namespace

namespace
{

void enqueue_scoring(Pipeline &p)
{
    sycl::queue &q = *p.queue;
    const Geometry &g = p.config.geometry;
    const Scoring &s = p.config.scoring;
    /* Channels come in (reference, distorted) pairs, one score per pair. */
    assert(p.config.channels % 2u == 0u && p.config.channels / 2u <= kMaxPairs);
    assert(g.blocks == g.blocks_h * (g.trunc_h / kBlock));
    assert(s.weight_mode >= 0 && s.weight_mode <= 6);
    const SolveArgs solve{.indterm = p.indterm,
                          .qmat = p.qmat,
                          .rmat = p.rmat,
                          .eig = p.eig,
                          .status = p.status,
                          .var = p.var,
                          .ent = p.ent,
                          .blocks = g.blocks,
                          .sigma_nn = s.sigma_nn,
                          .entropy_constant = s.entropy_constant};
    launch_solve(q, solve, p.config.channels);
    const ScoreArgs score{.var = p.var,
                          .ent = p.ent,
                          .status = p.status,
                          .contrib = p.contrib,
                          .result = p.result_device,
                          .blocks = g.blocks,
                          .base_entropy = s.base_entropy,
                          .weight_mode = s.weight_mode};
    launch_score(q, score, p.config.channels / 2u);
}

/* The device part of one frame, from the raw planes to FrameResult. */
void enqueue_chain(Pipeline &p, const ChannelBinding *bindings)
{
    const RawPlanes planes = bind_planes(p, bindings);
    if (p.config.geometry.bytes_per_sample == 2u) {
        enqueue_filter<uint16_t>(p, planes);
    } else {
        enqueue_filter<uint8_t>(p, planes);
    }
    enqueue_statistics(p);
    enqueue_scoring(p);
}

bool same_bindings(const ChannelBinding *a, const ChannelBinding *b, uint32_t channels)
{
    for (uint32_t ch = 0; ch < channels; ch++) {
        if (a[ch].minuend != b[ch].minuend || a[ch].subtrahend != b[ch].subtrahend) {
            return false;
        }
    }
    return true;
}

} // namespace

namespace
{

#if defined(SYCL_EXT_ONEAPI_GRAPH)
/* Leave recording mode after a failed recording, whatever state it failed in. */
void stop_recording(syclex::command_graph<syclex::graph_state::modifiable> &graph)
{
    try {
        graph.end_recording();
    } catch (const sycl::exception &e) {
        vmaf_log(VMAF_LOG_LEVEL_DEBUG, "speed_sycl: end_recording: %s\n", e.what());
    }
}

/* Record the chain for `bindings` once; later frames replay it as a single
 * submission instead of eight kernel launches. Returns the graph slot, or -1
 * when the device or runtime cannot record, in which case the pipeline keeps
 * enqueueing the kernels directly. */
int32_t recorded_chain(Pipeline &p, const ChannelBinding *bindings)
{
    assert(bindings != nullptr);
    assert(p.graph_count <= kMaxGraphs && p.config.channels <= kMaxChannels);
    for (uint32_t i = 0; i < p.graph_count; i++) {
        if (same_bindings(p.graph_bindings[i], bindings, p.config.channels)) {
            return static_cast<int32_t>(i);
        }
    }
    sycl::queue &q = *p.queue;
    const sycl::device device = q.get_device();
    if (p.graphs_disabled || p.graph_count == kMaxGraphs ||
        !(device.has(sycl::aspect::ext_oneapi_graph) ||
          device.has(sycl::aspect::ext_oneapi_limited_graph))) {
        return -1;
    }
    syclex::command_graph<syclex::graph_state::modifiable> graph(q.get_context(), device);
    try {
        graph.begin_recording(q);
        enqueue_chain(p, bindings);
        graph.end_recording(q);
        p.graphs[p.graph_count] = new ExecGraph(graph.finalize());
    } catch (const sycl::exception &e) {
        p.graphs_disabled = true;
        vmaf_log(VMAF_LOG_LEVEL_DEBUG, "speed_sycl: graph recording unavailable: %s\n", e.what());
        stop_recording(graph);
        return -1;
    }
    std::memcpy(p.graph_bindings[p.graph_count], bindings,
                sizeof(ChannelBinding) * p.config.channels);
    return static_cast<int32_t>(p.graph_count++);
}
#endif

} // namespace

namespace
{

void enqueue_frame(Pipeline &p, const ChannelBinding *bindings)
{
    sycl::queue &q = *p.queue;
#if defined(SYCL_EXT_ONEAPI_GRAPH)
    const int32_t slot = recorded_chain(p, bindings);
    if (slot >= 0) {
        /* Level Zero does not order a graph replay after plain in-order
         * commands on its own; the barriers pin the upload before it and the
         * result copy after it (same pattern as common.cpp). */
        q.ext_oneapi_submit_barrier();
        q.ext_oneapi_graph(*p.graphs[slot]);
        q.ext_oneapi_submit_barrier();
    } else {
        enqueue_chain(p, bindings);
    }
#else
    enqueue_chain(p, bindings);
#endif
    q.memcpy(p.result_host, p.result_device, sizeof(FrameResult));
}

} // namespace

int speed_sycl::pipeline_create(Pipeline **out, const PipelineConfig &config)
{
    if (!out || !config_valid(config)) {
        return -EINVAL;
    }
    auto *p = new (std::nothrow) Pipeline{};
    if (!p) {
        return -ENOMEM;
    }
    p->queue = static_cast<sycl::queue *>(config.queue);
    p->config = config;
    const Geometry &g = config.geometry;
    /* configure() sets 2 for the picture_copy() 16-bit path and 1 otherwise;
     * enqueue_chain() picks the sample type from exactly these two. */
    assert(g.bytes_per_sample == 1u || g.bytes_per_sample == 2u);
    p->plane_bytes = static_cast<size_t>(g.src_w) * g.src_h * g.bytes_per_sample;
    try {
        allocate_planes(*p);
        allocate_linalg(*p);
        if (!allocations_complete(*p)) {
            release_all(*p);
            delete p;
            return -ENOMEM;
        }
        upload_taps(*p);
    } catch (const sycl::exception &e) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "speed_sycl: pipeline setup failed: %s\n", e.what());
        release_all(*p);
        delete p;
        return -ENOMEM;
    }
    *out = p;
    return 0;
}

void speed_sycl::pipeline_destroy(Pipeline **pipeline)
{
    if (!pipeline || !*pipeline) {
        return;
    }
    Pipeline *p = *pipeline;
    try {
        p->queue->wait();
    } catch (const sycl::exception &e) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING, "speed_sycl: wait before release failed: %s\n", e.what());
    }
    release_all(*p);
    delete p;
    *pipeline = nullptr;
}

void *speed_sycl::pipeline_staging(Pipeline *pipeline, uint32_t index)
{
    if (!pipeline || index >= pipeline->config.staged) {
        return nullptr;
    }
    return pipeline->staging + static_cast<size_t>(index) * pipeline->plane_bytes;
}

int speed_sycl::pipeline_upload(Pipeline *pipeline, uint32_t first, uint32_t count)
{
    if (!pipeline || count == 0u || count > pipeline->config.staged ||
        first + count > pipeline->config.raw_planes) {
        return -EINVAL;
    }
    try {
        pipeline->queue->memcpy(pipeline->raw + static_cast<size_t>(first) * pipeline->plane_bytes,
                                pipeline->staging,
                                static_cast<size_t>(count) * pipeline->plane_bytes);
    } catch (const sycl::exception &e) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "speed_sycl: upload failed: %s\n", e.what());
        return -EIO;
    }
    return 0;
}

int speed_sycl::pipeline_submit(Pipeline *pipeline, const ChannelBinding *bindings)
{
    if (!pipeline || !bindings) {
        return -EINVAL;
    }
    for (uint32_t ch = 0; ch < pipeline->config.channels; ch++) {
        const int32_t planes = static_cast<int32_t>(pipeline->config.raw_planes);
        if (bindings[ch].minuend < 0 || bindings[ch].minuend >= planes ||
            bindings[ch].subtrahend >= planes) {
            return -EINVAL;
        }
    }
    try {
        enqueue_frame(*pipeline, bindings);
    } catch (const sycl::exception &e) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "speed_sycl: submit failed: %s\n", e.what());
        return -EIO;
    }
    return 0;
}

int speed_sycl::pipeline_wait(Pipeline *pipeline)
{
    if (!pipeline) {
        return -EINVAL;
    }
    try {
        pipeline->queue->wait_and_throw();
    } catch (const sycl::exception &e) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "speed_sycl: device chain failed: %s\n", e.what());
        return -EIO;
    }
    return 0;
}

int speed_sycl::pipeline_collect(Pipeline *pipeline, FrameResult *out)
{
    if (!out) {
        return -EINVAL;
    }
    const int err = pipeline_wait(pipeline);
    if (err) {
        return err;
    }
    *out = *pipeline->result_host;
    return 0;
}

int speed_sycl::stage_plane(Pipeline *pipeline, uint32_t index, const VmafPicture *pic,
                            unsigned plane)
{
    auto *dst = static_cast<unsigned char *>(pipeline_staging(pipeline, index));
    if (!dst || !pic || plane > 2u || !pic->data[plane]) {
        return -EINVAL;
    }
    const Geometry &g = pipeline->config.geometry;
    if (pic->w[plane] < g.src_w || pic->h[plane] < g.src_h) {
        return -EINVAL;
    }
    const size_t row_bytes = static_cast<size_t>(g.src_w) * g.bytes_per_sample;
    const auto *src = static_cast<const unsigned char *>(pic->data[plane]);
    const auto stride = static_cast<ptrdiff_t>(pic->stride[plane]);
    for (uint32_t row = 0; row < g.src_h; row++) {
        std::memcpy(dst + static_cast<size_t>(row) * row_bytes,
                    src + static_cast<ptrdiff_t>(row) * stride, row_bytes);
    }
    return 0;
}
