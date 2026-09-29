/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Exact fp32 building blocks for SYCL kernels that must reproduce host
 *  arithmetic without fp64 (ADR-0220): correctly rounded division and square
 *  root, and unevaluated fp32 pairs (hi + lo) with error-free transformations.
 *
 *  Moved out of speed_sycl_pipeline.cpp (ADR-1358) so the SpEED pipeline and
 *  the ssimulacra2 twin share one implementation (HISS-19, ADR-1363). Every
 *  function here assumes its translation unit is compiled with contraction
 *  off (`sycl_exact_fp_args` in core/src/meson.build): two_sum() and
 *  quick_two_sum() are exact only when each add rounds on its own, and the
 *  residual FMAs are written out with sycl::fma() so contraction never has to
 *  be relied on. This header must not mention the fp64 type
 *  (core/test/test_sycl_kernel_source_contract.py).
 */

#ifndef VMAF_FEATURE_SYCL_SYCL_EXACT_FP_H_
#define VMAF_FEATURE_SYCL_SYCL_EXACT_FP_H_

#include <sycl/sycl.hpp>
#if defined(SYCL_IMPLEMENTATION_ONEAPI)
/* <windows.h> (via compat/win32/pthread.h) defines min and max as macros,
 * which break the declarations in Intel's math headers on MSVC+SYCL. */
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max
#include <sycl/ext/intel/math.hpp>
#pragma pop_macro("max")
#pragma pop_macro("min")
#endif

#include <cstdint>
#include <limits>

namespace vmaf_sycl_exact
{

/* ------------------------------------------------------------------ */
/* Correctly rounded fp32 division and square root                     */
/* ------------------------------------------------------------------ */

/* Device `/` and sqrt() are not correctly rounded by default (measured: 28%
 * and 8% of random operands differ from the host), and the offload precision
 * flags only act when the final image is linked, which is shared with every
 * other extractor. Every division and square root below therefore goes
 * through div_rn() / sqrt_rn(): a hardware approximation refined once, then
 * the two adjacent floats around it are checked with exact FMA residuals.
 * When their residual signs prove the true value lies between them, the
 * nearer one (ties to even) is the correctly rounded result; otherwise, and
 * for zero, non-finite or extreme operands, the oneAPI math extension's
 * software round-to-nearest routine answers (exact, about 15x slower).
 * 0 mismatches against the host on 16.7M operands spanning 2^-126..2^127 on
 * B580 and UHD 770. AdaptiveCpp has no such extension: its slow path is the
 * plain operator, and the contract there is ADR-0214's tolerance.
 *
 * Operand ranges where every residual is exact: a dividend or radicand of at
 * least 2^-100 keeps the residual bits above the subnormal range, and a
 * normal divisor and quotient keep q * b finite. */
inline constexpr float kFastLow = 0x1p-100f;
inline constexpr float kNormalLow = 0x1p-126f;
inline constexpr float kFastHigh = 0x1p+126f;

inline bool in_fast_range(float v)
{
    const float magnitude = sycl::fabs(v);
    return magnitude >= kFastLow && magnitude <= kFastHigh;
}

inline bool in_normal_range(float v)
{
    const float magnitude = sycl::fabs(v);
    return magnitude >= kNormalLow && magnitude <= kFastHigh;
}

/* The math-extension routines are needed in device code only. A host copy of
 * a kernel body (DPC++ emits one per kernel lambda) would otherwise call their
 * host fallback in libsycl-devicelib-host.a, whose libm / fenv references do
 * not resolve in the static test links; host `/` and sqrt are correctly
 * rounded already. */
#if defined(SYCL_IMPLEMENTATION_ONEAPI) && defined(__SYCL_DEVICE_ONLY__)
#define VMAF_SYCL_EXACT_DEVICE_RN 1
#else
#define VMAF_SYCL_EXACT_DEVICE_RN 0
#endif

inline float slow_div_rn(float a, float b)
{
#if VMAF_SYCL_EXACT_DEVICE_RN
    return sycl::ext::intel::math::fdiv_rn(a, b);
#else
    return a / b;
#endif
}

inline float slow_sqrt_rn(float x)
{
#if VMAF_SYCL_EXACT_DEVICE_RN
    return sycl::ext::intel::math::fsqrt_rn(x);
#else
    return sycl::sqrt(x);
#endif
}

/* Adjacent float of a finite non-zero x, one step up or down in value. */
inline float step_float(float x, bool up)
{
    const auto bits = sycl::bit_cast<uint32_t>(x);
    const bool away_from_zero = (x > 0.0f) == up;
    return sycl::bit_cast<float>(away_from_zero ? bits + 1u : bits - 1u);
}

inline bool even_significand(float x)
{
    return (sycl::bit_cast<uint32_t>(x) & 1u) == 0u;
}

/* Correctly rounded a / b from a faithful q with exact residual r = a - q b,
 * or NaN when the bracket cannot be proved (the caller then takes the slow
 * path). */
inline float round_quotient(float a, float b, float q, float r)
{
    const float other = step_float(q, (r > 0.0f) == (b > 0.0f));
    const float r_other = sycl::fma(-other, b, a);
    if (r_other == 0.0f) {
        return other;
    }
    if ((r_other > 0.0f) == (r > 0.0f)) {
        return std::numeric_limits<float>::quiet_NaN(); /* a / b not between q and other */
    }
    /* Not near / far: <windows.h> defines both as empty macros. */
    const float dist_q = sycl::fabs(r);
    const float dist_other = sycl::fabs(r_other);
    if (dist_q != dist_other) {
        return dist_q < dist_other ? q : other;
    }
    return even_significand(q) ? q : other;
}

inline float div_rn(float a, float b)
{
    if (a == 0.0f && in_normal_range(b)) {
        return a / b; /* signed zero, exact */
    }
    const float y = sycl::native::recip(b);
    const float q0 = a * y;
    if (!in_fast_range(a) || !in_normal_range(b) || !in_normal_range(q0)) {
        return slow_div_rn(a, b);
    }
    const float q = sycl::fma(sycl::fma(-q0, b, a), y, q0);
    const float r = sycl::fma(-q, b, a); /* b * (a / b - q) */
    if (r == 0.0f) {
        return q;
    }
    const float rounded = round_quotient(a, b, q, r);
    return sycl::isnan(rounded) ? slow_div_rn(a, b) : rounded;
}

/* Choose between adjacent lo < hi around sqrt(x): hi iff x > ((lo + hi) / 2)^2,
 * i.e. r_lo = x - lo^2 > lo * w + w^2 / 4 with w = hi - lo a power of two.
 * The square root of a float is never exactly a midpoint. */
inline float nearer_root(float lo, float hi, float r_lo)
{
    const float w = hi - lo;
    const float c = lo * w;
    const float d = 0.25f * (w * w);
    if (r_lo < 0.5f * c) {
        return lo;
    }
    if (r_lo > 2.0f * c) {
        return hi;
    }
    const float excess = r_lo - c; /* exact (Sterbenz) */
    return excess > d ? hi : lo;
}

inline float sqrt_rn(float x)
{
    if (x == 0.0f) {
        return x;
    }
    if (!(x >= kFastLow && x <= kFastHigh)) {
        return slow_sqrt_rn(x);
    }
    const float s0 = sycl::native::sqrt(x);
    const float half_inverse = 0.5f * sycl::native::recip(s0);
    const float s = sycl::fma(sycl::fma(-s0, s0, x), half_inverse, s0);
    const float r = sycl::fma(-s, s, x);
    if (r == 0.0f) {
        return s;
    }
    const float other = step_float(s, r > 0.0f);
    const float r_other = sycl::fma(-other, other, x);
    if (r_other == 0.0f) {
        return other;
    }
    if ((r_other > 0.0f) == (r > 0.0f)) {
        return slow_sqrt_rn(x); /* sqrt(x) is not between s and other */
    }
    return r > 0.0f ? nearer_root(s, other, r) : nearer_root(other, s, r_other);
}

/* ------------------------------------------------------------------ */
/* Exact fp32 pair arithmetic                                          */
/* ------------------------------------------------------------------ */

struct Ff {
    float hi;
    float lo;
};

inline Ff two_sum(float a, float b)
{
    const float sum = a + b;
    const float b_virtual = sum - a;
    const float a_virtual = sum - b_virtual;
    const float b_error = b - b_virtual;
    const float a_error = a - a_virtual;
    return {.hi = sum, .lo = a_error + b_error};
}

inline Ff quick_two_sum(float a, float b)
{
    const float sum = a + b;
    const float rebuilt = sum - a;
    return {.hi = sum, .lo = b - rebuilt};
}

inline Ff two_prod(float a, float b)
{
    const float product = a * b;
    return {.hi = product, .lo = sycl::fma(a, b, -product)};
}

inline Ff ff_add(Ff a, Ff b)
{
    const Ff high = two_sum(a.hi, b.hi);
    const Ff low = two_sum(a.lo, b.lo);
    const Ff first = quick_two_sum(high.hi, high.lo + low.hi);
    return quick_two_sum(first.hi, low.lo + first.lo);
}

inline Ff ff_mul(Ff a, Ff b)
{
    const Ff product = two_prod(a.hi, b.hi);
    const float cross1 = a.hi * b.lo;
    const float cross2 = a.lo * b.hi;
    const float cross = cross1 + cross2;
    return quick_two_sum(product.hi, product.lo + cross);
}

/* (hi + lo) / divisor, rounded once to fp32. */
inline float ff_div_to_float(Ff value, float divisor)
{
    const float quotient = div_rn(value.hi, divisor);
    const float remainder = sycl::fma(-quotient, divisor, value.hi);
    const float correction = div_rn(remainder + value.lo, divisor);
    return quotient + correction;
}

inline Ff ff_neg(Ff a)
{
    return {.hi = -a.hi, .lo = -a.lo};
}

/* a / b as a pair, relative error about 2^-46 (one quotient digit per step,
 * the residual a - q1 * b formed with ff_mul / ff_add). Both partial
 * quotients are correctly rounded, so the result does not depend on the
 * device's division unit. A non-finite first quotient is returned as is
 * (lo = 0) so infinities and NaN reach the caller unchanged. */
inline Ff ff_div(Ff a, Ff b)
{
    const float q1 = div_rn(a.hi, b.hi);
    if (!sycl::isfinite(q1)) {
        return {.hi = q1, .lo = 0.0f};
    }
    const Ff scaled = ff_mul(b, Ff{.hi = q1, .lo = 0.0f});
    const Ff residual = ff_add(a, ff_neg(scaled));
    const float q2 = div_rn(residual.hi, b.hi);
    return quick_two_sum(q1, q2);
}

} // namespace vmaf_sycl_exact

#endif /* VMAF_FEATURE_SYCL_SYCL_EXACT_FP_H_ */
