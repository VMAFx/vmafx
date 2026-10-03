/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Exact fp32 pair arithmetic for a backend whose fp32 `+`, `-`, `*` and `/`
 *  are IEEE operations rounded to nearest and that has a fused multiply-add:
 *  the operations ff_math.h builds on (`vmaf_ffm_base`). A pair (hi + lo)
 *  carries about 48 significant bits.
 *
 *  The including file defines first:
 *
 *    VMAF_FF_INLINE         the specifier of every function here
 *    VMAF_FF_FMA(a, b, c)   a * b + c with one rounding
 *
 *  and compiles with contraction off: two_sum() and quick_two_sum() are
 *  error-free only when every operation in them rounds on its own.
 *
 *  The SYCL backend does not use this header: its device division is not
 *  correctly rounded, so it has operations of its own (sycl/sycl_exact_fp.h),
 *  among them the same error-free sums and products.
 *
 *  A backend that compiles the subset Metal Shading Language and host C++
 *  share defines VMAF_FF_MSL_SUBSET as well (feature/metal/metal_ciede_math.h,
 *  ADR-1498): Metal's language revision is C++14-based and has no designated
 *  initializers, so a pair is built with positional ones there, which name
 *  the same members in the same order (hi, lo). The other backends compile
 *  the designated form, unchanged.
 */

#ifndef VMAF_FEATURE_FF_PAIR_H_
#define VMAF_FEATURE_FF_PAIR_H_

#if !defined(VMAF_FF_INLINE) || !defined(VMAF_FF_FMA)
#error "ff_pair.h: define VMAF_FF_INLINE and VMAF_FF_FMA before including it"
#endif

namespace vmaf_ff_pair
{

struct Ff {
    float hi;
    float lo;
};

/* a + b, exactly: the rounded sum and what the rounding dropped. */
VMAF_FF_INLINE Ff two_sum(float a, float b)
{
    const float sum = a + b;
    const float b_virtual = sum - a;
    const float a_virtual = sum - b_virtual;
    const float b_error = b - b_virtual;
    const float a_error = a - a_virtual;
#if defined(VMAF_FF_MSL_SUBSET)
    // NOLINTNEXTLINE(modernize-use-designated-initializers): MSL has none, ADR-1498
    return {sum, a_error + b_error};
#else
    return {.hi = sum, .lo = a_error + b_error};
#endif
}

/* two_sum() for |a| >= |b|. */
VMAF_FF_INLINE Ff quick_two_sum(float a, float b)
{
    const float sum = a + b;
    const float rebuilt = sum - a;
#if defined(VMAF_FF_MSL_SUBSET)
    // NOLINTNEXTLINE(modernize-use-designated-initializers): MSL has none, ADR-1498
    return {sum, b - rebuilt};
#else
    return {.hi = sum, .lo = b - rebuilt};
#endif
}

/* a * b, exactly. */
VMAF_FF_INLINE Ff two_prod(float a, float b)
{
    const float product = a * b;
#if defined(VMAF_FF_MSL_SUBSET)
    // NOLINTNEXTLINE(modernize-use-designated-initializers): MSL has none, ADR-1498
    return {product, VMAF_FF_FMA(a, b, -product)};
#else
    return {.hi = product, .lo = VMAF_FF_FMA(a, b, -product)};
#endif
}

VMAF_FF_INLINE Ff ff_add(Ff a, Ff b)
{
    const Ff high = two_sum(a.hi, b.hi);
    const Ff low = two_sum(a.lo, b.lo);
    const Ff first = quick_two_sum(high.hi, high.lo + low.hi);
    return quick_two_sum(first.hi, low.lo + first.lo);
}

VMAF_FF_INLINE Ff ff_mul(Ff a, Ff b)
{
    const Ff product = two_prod(a.hi, b.hi);
    const float cross1 = a.hi * b.lo;
    const float cross2 = a.lo * b.hi;
    const float cross = cross1 + cross2;
    return quick_two_sum(product.hi, product.lo + cross);
}

VMAF_FF_INLINE Ff ff_neg(Ff a)
{
#if defined(VMAF_FF_MSL_SUBSET)
    // NOLINTNEXTLINE(modernize-use-designated-initializers): MSL has none, ADR-1498
    return {-a.hi, -a.lo};
#else
    return {.hi = -a.hi, .lo = -a.lo};
#endif
}

/* The correctly rounded fp32 quotient: the backend's own `/`. */
VMAF_FF_INLINE float div_rn(float a, float b)
{
    return a / b;
}

} // namespace vmaf_ff_pair

#endif /* VMAF_FEATURE_FF_PAIR_H_ */
