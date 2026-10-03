/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  fp64 operations in 64-bit integers, for Metal kernels that have to return
 *  what a CPU reference computes in `double` (Metal has no fp64 type, Metal
 *  Shading Language Specification 4.1, section 2.1). A VmafMtlSoftDouble is
 *  a positive fp64 value as its 53-bit significand and an exponent; every
 *  operation here rounds to nearest, ties to even, as the fp64 operation it
 *  stands for, so a sequence of them reproduces the reference's sequence bit
 *  for bit.
 *
 *  This is core/src/feature/sycl/sycl_soft_double.h statement for statement
 *  (ADR-1498): the same algorithms, the same rounding, the same domains. The
 *  SYCL header is written against sycl:: and C++20 and cannot be included
 *  from Metal; the two are one behaviour in two files until
 *  T-METAL-SYCL-FP64-FREE-ARITHMETIC-COPIES-2026-10-03 merges them, and a
 *  change to one changes the other in the same PR.
 *
 *  Written on metal_portable.h in what Metal Shading Language, C and C++
 *  share: values in and values out, typedef'd structs built by the _make()
 *  functions (no designated initializers), constants as macros, no `double`,
 *  no `long long`, no 128-bit type (the 64x64 product is written out in
 *  32-bit limbs, as the SYCL header does). core/test/test_metal_soft_double.cpp
 *  holds every function against the host's fp64 arithmetic;
 *  core/test/test_metal_soft_double_contract.py pins the subset and the
 *  mapping below.
 *
 *  Kernel-safe: integers only, no arrays, and no select between structs
 *  (scalar selects only, as in the SYCL header).
 *
 *  They are slow: a division is 56 loop steps. A twin calls them for the rare
 *  sample its faster arithmetic cannot decide, never for every pixel, unless
 *  its SYCL twin does.
 *
 *  Name mapping, SYCL (namespace vmaf_sycl_soft) -> Metal:
 *    struct SoftDouble{mant, exp}  -> VmafMtlSoftDouble, built by
 *                                     vmaf_mtl_soft_make(mant, exp)
 *    kDoubleTop                    -> VMAF_MTL_SOFT_DOUBLE_TOP
 *    struct U128{hi, lo}           -> VmafMtlU128, vmaf_mtl_u128_make(hi, lo)
 *    u128_shl, u128_sub, u128_mul,
 *    u128_msb, u128_shr_round      -> vmaf_mtl_u128_shl, ... (prefix vmaf_mtl_)
 *    struct Shifted{kept, half,
 *                   sticky}        -> VmafMtlShifted,
 *                                     vmaf_mtl_shifted_make(kept, half, sticky)
 *    round_kept                    -> vmaf_mtl_round_kept
 *    soft_<op>                     -> vmaf_mtl_soft_<op> (from_float, from_u32,
 *                                     round, add, div, mul, less, trunc,
 *                                     to_float, from_float_any, to_float_any,
 *                                     sub_trunc)
 *    sycl::clz(uint32_t)           -> vmaf_mtl_soft_clz32
 *    sycl::bit_cast<>              -> VMAF_MTL_F2U / VMAF_MTL_U2F
 */

#ifndef VMAF_FEATURE_METAL_METAL_SOFT_DOUBLE_H_
#define VMAF_FEATURE_METAL_METAL_SOFT_DOUBLE_H_

#include "metal_portable.h"

#if !defined(__METAL_VERSION__) && !defined(__cplusplus)
#include <stdbool.h>
#endif

/* A positive fp64 value, mant * 2^exp with mant in [2^52, 2^53). */
typedef struct VmafMtlSoftDouble {
    vmaf_mtl_u64 mant;
    vmaf_mtl_i32 exp;
} VmafMtlSoftDouble;

#define VMAF_MTL_SOFT_DOUBLE_TOP (VMAF_MTL_U64(1) << 52)

VMAF_MTL_FUNC VmafMtlSoftDouble vmaf_mtl_soft_make(vmaf_mtl_u64 mant, vmaf_mtl_i32 exp)
{
    const VmafMtlSoftDouble value = {mant, exp};
    return value;
}

/* Leading zero bits of a 32-bit value; 32 for 0 (sycl::clz on uint32_t). */
VMAF_MTL_FUNC vmaf_mtl_u32 vmaf_mtl_soft_clz32(vmaf_mtl_u32 x)
{
    return VMAF_MTL_CLZ64(VMAF_MTL_U64(x)) - 32u;
}

/* ------------------------------------------------------------------ */
/* 128-bit helpers                                                     */
/* ------------------------------------------------------------------ */

/* A 128-bit unsigned value. */
typedef struct VmafMtlU128 {
    vmaf_mtl_u64 hi;
    vmaf_mtl_u64 lo;
} VmafMtlU128;

VMAF_MTL_FUNC VmafMtlU128 vmaf_mtl_u128_make(vmaf_mtl_u64 hi, vmaf_mtl_u64 lo)
{
    const VmafMtlU128 value = {hi, lo};
    return value;
}

VMAF_MTL_FUNC VmafMtlU128 vmaf_mtl_u128_shl(vmaf_mtl_u64 value, vmaf_mtl_u32 shift)
{
    /* shift < 128 */
    const vmaf_mtl_u64 lo = shift < 64u ? value << shift : VMAF_MTL_U64(0);
    const vmaf_mtl_u64 spill =
        (shift == 0u || shift >= 64u) ? VMAF_MTL_U64(0) : value >> (64u - shift);
    const vmaf_mtl_u64 hi = shift >= 64u ? value << (shift - 64u) : spill;
    return vmaf_mtl_u128_make(hi, lo);
}

VMAF_MTL_FUNC VmafMtlU128 vmaf_mtl_u128_sub(VmafMtlU128 a, vmaf_mtl_u64 b)
{
    const vmaf_mtl_u64 lo = a.lo - b;
    return vmaf_mtl_u128_make(a.hi - (a.lo < b ? VMAF_MTL_U64(1) : VMAF_MTL_U64(0)), lo);
}

/* a * b in full. Written out in 32-bit limbs, as the SYCL header does
 * (sycl::mul_hi() on 64-bit operands returned wrong values in a kernel on an
 * Arc A380); the Metal spelling of mulhi() on 64-bit operands is not used. */
VMAF_MTL_FUNC VmafMtlU128 vmaf_mtl_u128_mul(vmaf_mtl_u64 a, vmaf_mtl_u64 b)
{
    const vmaf_mtl_u64 mask = 0xFFFFFFFFu;
    const vmaf_mtl_u64 a_lo = a & mask;
    const vmaf_mtl_u64 a_hi = a >> 32;
    const vmaf_mtl_u64 b_lo = b & mask;
    const vmaf_mtl_u64 b_hi = b >> 32;
    const vmaf_mtl_u64 low = a_lo * b_lo;
    const vmaf_mtl_u64 cross1 = a_lo * b_hi;
    const vmaf_mtl_u64 cross2 = a_hi * b_lo;
    const vmaf_mtl_u64 high = a_hi * b_hi;
    const vmaf_mtl_u64 middle = (low >> 32) + (cross1 & mask) + (cross2 & mask);
    return vmaf_mtl_u128_make(high + (cross1 >> 32) + (cross2 >> 32) + (middle >> 32),
                              (low & mask) | (middle << 32));
}

/* Position of the highest set bit; the value is not zero. */
VMAF_MTL_FUNC vmaf_mtl_u32 vmaf_mtl_u128_msb(VmafMtlU128 a)
{
    const vmaf_mtl_u32 hi_bits = 127u - VMAF_MTL_CLZ64(a.hi | VMAF_MTL_U64(1));
    const vmaf_mtl_u32 lo_bits = 63u - VMAF_MTL_CLZ64(a.lo | VMAF_MTL_U64(1));
    return a.hi != 0u ? hi_bits : lo_bits;
}

/* a >> shift (shift < 128), and whether any dropped bit was set / the value
 * of the highest dropped bit. */
typedef struct VmafMtlShifted {
    vmaf_mtl_u64 kept;
    bool half;   /* highest dropped bit */
    bool sticky; /* any lower dropped bit */
} VmafMtlShifted;

VMAF_MTL_FUNC VmafMtlShifted vmaf_mtl_shifted_make(vmaf_mtl_u64 kept, bool half, bool sticky)
{
    const VmafMtlShifted value = {kept, half, sticky};
    return value;
}

VMAF_MTL_FUNC VmafMtlShifted vmaf_mtl_u128_shr_round(VmafMtlU128 a, vmaf_mtl_u32 shift)
{
    if (shift == 0u) {
        return vmaf_mtl_shifted_make(a.lo, false, false);
    }
    /* the highest dropped bit is bit shift - 1 */
    const vmaf_mtl_u32 half_bit = shift - 1u;
    const bool half =
        half_bit >= 64u ? ((a.hi >> (half_bit - 64u)) & 1u) != 0u : ((a.lo >> half_bit) & 1u) != 0u;
    bool sticky = false;
    if (half_bit >= 64u) {
        const vmaf_mtl_u64 below_hi =
            half_bit == 64u ? VMAF_MTL_U64(0) : a.hi & ((VMAF_MTL_U64(1) << (half_bit - 64u)) - 1u);
        sticky = below_hi != 0u || a.lo != 0u;
    } else {
        const vmaf_mtl_u64 below =
            half_bit == 0u ? VMAF_MTL_U64(0) : a.lo & ((VMAF_MTL_U64(1) << half_bit) - 1u);
        sticky = below != 0u;
    }
    vmaf_mtl_u64 kept = 0u;
    if (shift >= 64u) {
        kept = a.hi >> (shift - 64u);
    } else {
        kept = (a.lo >> shift) | (a.hi << (64u - shift));
    }
    return vmaf_mtl_shifted_make(kept, half, sticky);
}

/* Round to nearest, ties to even. */
VMAF_MTL_FUNC vmaf_mtl_u64 vmaf_mtl_round_kept(VmafMtlShifted s)
{
    const bool up = s.half && (s.sticky || (s.kept & 1u) != 0u);
    return s.kept + (up ? VMAF_MTL_U64(1) : VMAF_MTL_U64(0));
}

/* ------------------------------------------------------------------ */
/* The operations                                                      */
/* ------------------------------------------------------------------ */

/* A positive normal fp32 value as a VmafMtlSoftDouble (exact). */
VMAF_MTL_FUNC VmafMtlSoftDouble vmaf_mtl_soft_from_float(float x)
{
    const vmaf_mtl_u32 bits = VMAF_MTL_F2U(x);
    const vmaf_mtl_u64 mant = VMAF_MTL_U64((bits & 0x007FFFFFu) | 0x00800000u) << 29;
    return vmaf_mtl_soft_make(mant, (vmaf_mtl_i32)((bits >> 23) & 0xFFu) - 150 - 29);
}

/* A positive integer below 2^32 as a VmafMtlSoftDouble (exact). */
VMAF_MTL_FUNC VmafMtlSoftDouble vmaf_mtl_soft_from_u32(vmaf_mtl_u32 x)
{
    const vmaf_mtl_u32 shift = 21u + vmaf_mtl_soft_clz32(x);
    return vmaf_mtl_soft_make(VMAF_MTL_U64(x) << shift, -(vmaf_mtl_i32)shift);
}

/* Round `mant` (below 2^56, three extra bits at the bottom, the last one
 * sticky) to 53 bits, ties to even. */
VMAF_MTL_FUNC VmafMtlSoftDouble vmaf_mtl_soft_round(vmaf_mtl_u64 mant, vmaf_mtl_i32 exp)
{
    const vmaf_mtl_u64 low = mant & 7u;
    vmaf_mtl_u64 kept = mant >> 3;
    if (low > 4u || (low == 4u && (kept & 1u) != 0u)) {
        kept += 1u;
    }
    if (kept == (VMAF_MTL_SOFT_DOUBLE_TOP << 1)) {
        return vmaf_mtl_soft_make(VMAF_MTL_SOFT_DOUBLE_TOP, exp + 4);
    }
    return vmaf_mtl_soft_make(kept, exp + 3);
}

/* fl64(a + b) for positive a and b. */
VMAF_MTL_FUNC VmafMtlSoftDouble vmaf_mtl_soft_add(VmafMtlSoftDouble a, VmafMtlSoftDouble b)
{
    /* Scalar selects, not a select of the structs: a struct select stays in
     * private memory on a SYCL device (ADR-1395); kept for the same shape. */
    const bool swap = (b.exp > a.exp) || (b.exp == a.exp && b.mant > a.mant);
    const vmaf_mtl_u64 big_mant = swap ? b.mant : a.mant;
    const vmaf_mtl_u64 small_mant = swap ? a.mant : b.mant;
    const vmaf_mtl_i32 big_exp = swap ? b.exp : a.exp;
    const vmaf_mtl_i32 small_exp = swap ? a.exp : b.exp;
    /* Beyond 59 places the smaller term is below the last kept bit and only
     * sets the sticky bit; 59 gives the same sum. */
    const vmaf_mtl_u32 distance = (vmaf_mtl_u32)(big_exp - small_exp);
    const vmaf_mtl_u32 shift = distance < 59u ? distance : 59u;
    const vmaf_mtl_u64 small_wide = small_mant << 3;
    const vmaf_mtl_u64 lost = small_wide & ((VMAF_MTL_U64(1) << shift) - 1u);
    const vmaf_mtl_u64 aligned =
        (small_wide >> shift) | (lost != 0u ? VMAF_MTL_U64(1) : VMAF_MTL_U64(0));
    vmaf_mtl_u64 sum = (big_mant << 3) + aligned;
    vmaf_mtl_i32 exp = big_exp - 3;
    if (sum >= (VMAF_MTL_SOFT_DOUBLE_TOP << 4)) {
        sum = (sum >> 1) | (sum & 1u);
        exp += 1;
    }
    return vmaf_mtl_soft_round(sum, exp);
}

/* fl64(a / b) for positive a and b. Restoring division, one quotient bit per
 * step: 56 bits of quotient and the remainder as the sticky bit. */
VMAF_MTL_FUNC VmafMtlSoftDouble vmaf_mtl_soft_div(VmafMtlSoftDouble a, VmafMtlSoftDouble b)
{
    vmaf_mtl_u64 rem = a.mant;
    vmaf_mtl_i32 exp = a.exp - b.exp - 55;
    if (rem < b.mant) {
        rem <<= 1;
        exp -= 1;
    }
    vmaf_mtl_u64 quot = 0u;
    for (int step = 0; step < 56; step++) {
        const bool take = rem >= b.mant;
        rem -= take ? b.mant : VMAF_MTL_U64(0);
        quot = (quot << 1) | (take ? VMAF_MTL_U64(1) : VMAF_MTL_U64(0));
        rem <<= 1;
    }
    quot |= rem != 0u ? VMAF_MTL_U64(1) : VMAF_MTL_U64(0);
    return vmaf_mtl_soft_round(quot, exp);
}

/* fl64(a * b). */
VMAF_MTL_FUNC VmafMtlSoftDouble vmaf_mtl_soft_mul(VmafMtlSoftDouble a, VmafMtlSoftDouble b)
{
    const VmafMtlU128 product = vmaf_mtl_u128_mul(a.mant, b.mant);
    /* In [2^104, 2^106): 105 or 106 bits. */
    const vmaf_mtl_u32 shift = (product.hi >> 41) != 0u ? 53u : 52u;
    vmaf_mtl_u64 mant = vmaf_mtl_round_kept(vmaf_mtl_u128_shr_round(product, shift));
    vmaf_mtl_i32 exp = a.exp + b.exp + (vmaf_mtl_i32)shift;
    if (mant == (VMAF_MTL_SOFT_DOUBLE_TOP << 1)) {
        mant = VMAF_MTL_SOFT_DOUBLE_TOP;
        exp += 1;
    }
    return vmaf_mtl_soft_make(mant, exp);
}

VMAF_MTL_FUNC bool vmaf_mtl_soft_less(VmafMtlSoftDouble a, VmafMtlSoftDouble b)
{
    return a.exp < b.exp || (a.exp == b.exp && a.mant < b.mant);
}

/* (int64_t)value: truncation. The value is below 2^63. */
VMAF_MTL_FUNC vmaf_mtl_i64 vmaf_mtl_soft_trunc(VmafMtlSoftDouble a)
{
    if (a.exp >= 0) {
        return (vmaf_mtl_i64)(a.mant << (vmaf_mtl_u32)a.exp);
    }
    const vmaf_mtl_u32 shift = (vmaf_mtl_u32)(-a.exp);
    return shift >= 64u ? VMAF_MTL_I64(0) : (vmaf_mtl_i64)(a.mant >> shift);
}

/* (float)value, ties to even. The value is at least 1 and far below the
 * fp32 range's end here. */
VMAF_MTL_FUNC float vmaf_mtl_soft_to_float(VmafMtlSoftDouble value)
{
    const vmaf_mtl_u64 low = value.mant & ((VMAF_MTL_U64(1) << 29) - 1u);
    const vmaf_mtl_u64 half = VMAF_MTL_U64(1) << 28;
    vmaf_mtl_u64 kept = value.mant >> 29;
    vmaf_mtl_i32 exp = value.exp + 29;
    if (low > half || (low == half && (kept & 1u) != 0u)) {
        kept += 1u;
    }
    if (kept == (VMAF_MTL_U64(1) << 24)) {
        kept >>= 1;
        exp += 1;
    }
    const vmaf_mtl_u32 biased = (vmaf_mtl_u32)(exp + 150);
    return VMAF_MTL_U2F((biased << 23) | ((vmaf_mtl_u32)kept & 0x007FFFFFu));
}

/* A positive finite fp32 value, normal or subnormal, as a VmafMtlSoftDouble
 * (exact). A subnormal is fraction * 2^-149 without an implicit bit; its
 * leading bit is moved to the top. */
VMAF_MTL_FUNC VmafMtlSoftDouble vmaf_mtl_soft_from_float_any(float x)
{
    const vmaf_mtl_u32 bits = VMAF_MTL_F2U(x);
    const vmaf_mtl_u32 biased = (bits >> 23) & 0xFFu;
    const vmaf_mtl_u32 fraction = bits & 0x007FFFFFu;
    const vmaf_mtl_u32 sub_shift = 21u + vmaf_mtl_soft_clz32(fraction | 1u);
    const vmaf_mtl_u64 sub_mant = VMAF_MTL_U64(fraction) << sub_shift;
    const vmaf_mtl_i32 sub_exp = -149 - (vmaf_mtl_i32)sub_shift;
    const vmaf_mtl_u64 normal_mant = VMAF_MTL_U64(fraction | 0x00800000u) << 29;
    const vmaf_mtl_i32 normal_exp = (vmaf_mtl_i32)biased - 150 - 29;
    const bool subnormal = biased == 0u;
    return vmaf_mtl_soft_make(subnormal ? sub_mant : normal_mant, subnormal ? sub_exp : normal_exp);
}

/* (float)value for any positive value: ties to even, a subnormal or zero
 * result below the normal range, infinity above it. */
VMAF_MTL_FUNC float vmaf_mtl_soft_to_float_any(VmafMtlSoftDouble value)
{
    vmaf_mtl_i32 lead = value.exp + 52; /* the exponent of the leading bit */
    const bool normal = lead >= -126;
    /* A normal result keeps 24 bits; a subnormal one keeps what lies at or
     * above 2^-149. */
    const vmaf_mtl_i32 sub_drop = -149 - value.exp;
    if (!normal && sub_drop > 53) {
        return 0.0f;
    }
    const vmaf_mtl_u32 drop = normal ? 29u : (vmaf_mtl_u32)sub_drop;
    const vmaf_mtl_u64 half = VMAF_MTL_U64(1) << (drop - 1u);
    const vmaf_mtl_u64 low = value.mant & ((half << 1) - 1u);
    vmaf_mtl_u64 kept = value.mant >> drop;
    if (low > half || (low == half && (kept & 1u) != 0u)) {
        kept += 1u;
    }
    if (!normal) {
        /* 2^23 here is the smallest normal value, with the same bits. */
        return VMAF_MTL_U2F((vmaf_mtl_u32)kept);
    }
    if (kept == (VMAF_MTL_U64(1) << 24)) {
        kept >>= 1;
        lead += 1;
    }
    if (lead > 127) {
        return VMAF_MTL_U2F(0x7F800000u);
    }
    const vmaf_mtl_u32 biased = (vmaf_mtl_u32)(lead + 127);
    return VMAF_MTL_U2F((biased << 23) | ((vmaf_mtl_u32)kept & 0x007FFFFFu));
}

/* trunc(fl64(a - t)) for an integer a below 2^31 and 0 < t < a. */
VMAF_MTL_FUNC vmaf_mtl_u32 vmaf_mtl_soft_sub_trunc(vmaf_mtl_u32 a, VmafMtlSoftDouble t)
{
    if (t.exp >= 0) {
        /* t is an integer: the difference is exact. */
        return a - (vmaf_mtl_u32)(t.mant << (vmaf_mtl_u32)t.exp);
    }
    const vmaf_mtl_u32 frac_bits = (vmaf_mtl_u32)(-t.exp); /* at most 84: t >= 2^-31 */
    const VmafMtlU128 exact =
        vmaf_mtl_u128_sub(vmaf_mtl_u128_shl(VMAF_MTL_U64(a), frac_bits), t.mant);
    const vmaf_mtl_u32 msb = vmaf_mtl_u128_msb(exact);
    /* The difference is exact * 2^-frac_bits. Round it to 53 bits. */
    const vmaf_mtl_u32 drop = msb > 52u ? msb - 52u : 0u;
    const vmaf_mtl_u64 mant = vmaf_mtl_round_kept(vmaf_mtl_u128_shr_round(exact, drop));
    /* value = mant * 2^(drop - frac_bits); mant may be 2^53 after rounding. */
    if (drop >= frac_bits) {
        return (vmaf_mtl_u32)(mant << (drop - frac_bits));
    }
    const vmaf_mtl_u32 shift = frac_bits - drop;
    return shift >= 64u ? 0u : (vmaf_mtl_u32)(mant >> shift);
}

#endif /* VMAF_FEATURE_METAL_METAL_SOFT_DOUBLE_H_ */
