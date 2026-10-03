/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Signed fp64 values in 64-bit integers, for a Metal kernel that has to
 *  return the `double` a CPU reference computes (Metal has no fp64 type).
 *  metal_soft_double.h has the positive values and their product, quotient
 *  and sum; this header adds the sign, zero, the difference (with its
 *  cancellation) and the conversions from an integer and to the IEEE-754 bit
 *  pattern. Every operation rounds to nearest, ties to even, as the fp64
 *  operation it stands for, so a sequence of them is the reference's sequence
 *  bit for bit.
 *
 *  This is core/src/feature/sycl/sycl_soft_signed.h statement for statement
 *  (ADR-1498), in the subset metal_soft_double.h describes; the two are one
 *  behaviour in two files until
 *  T-METAL-SYCL-FP64-FREE-ARITHMETIC-COPIES-2026-10-03 merges them.
 *
 *  Range: normal values and zero. No subnormal, infinity or NaN is formed or
 *  accepted; a caller's values stay far inside the fp64 range. A zero has no
 *  sign: -0.0 enters and leaves as +0.0.
 *
 *  Kernel-safe: integers only, no arrays, and scalar selects (no select
 *  between structs).
 *
 *  Name mapping, SYCL (namespace vmaf_sycl_soft) -> Metal:
 *    struct SoftSigned{mant, exp,
 *                      negative}   -> VmafMtlSoftSigned (raw fields; build a
 *                                     value with vmaf_mtl_signed_make())
 *    kZeroExp, kFractionMask,
 *    kQuietNanBits, kExponentBias,
 *    kExponentFieldMask,
 *    kMantissaBits, kGuardBits,
 *    kGuardedTopBit, kDigitBits,
 *    kDigitScale, kBelowFloat      -> VMAF_MTL_SOFT_ZERO_EXP,
 *                                     VMAF_MTL_SOFT_FRACTION_MASK, ...
 *                                     (k<Name> -> VMAF_MTL_SOFT_<NAME>)
 *    signed_<op>                   -> vmaf_mtl_signed_<op> (make, from_u64,
 *                                     from_exact, from_float, from_bits, bits,
 *                                     negate, abs, twice, add, sub, mul, div)
 *    struct DivStep{rem, digit}    -> VmafMtlDivStep
 *    div_step                      -> vmaf_mtl_div_step
 *    soft_div_digits               -> vmaf_mtl_soft_div_digits
 *    VMAF_SYCL_ALWAYS_INLINE       -> VMAF_MTL_FUNC
 *    SoftDouble{.mant, .exp}       -> vmaf_mtl_soft_make(mant, exp)
 *    a variable `half`             -> `halfway` (half is an MSL scalar type)
 */

#ifndef VMAF_FEATURE_METAL_METAL_SOFT_SIGNED_H_
#define VMAF_FEATURE_METAL_METAL_SOFT_SIGNED_H_

#include "metal_portable.h"
#include "metal_soft_double.h"

/* An fp64 value: (negative ? -1 : 1) * mant * 2^exp with mant in
 * [2^52, 2^53), or zero (mant 0, exp VMAF_MTL_SOFT_ZERO_EXP, not negative). */
// NOLINTNEXTLINE(modernize-use-using): C and MSL share this header, ADR-1498
typedef struct VmafMtlSoftSigned {
    vmaf_mtl_u64 mant;
    vmaf_mtl_i32 exp;
    vmaf_mtl_u32 negative;
} VmafMtlSoftSigned;

/* Zero's exponent: below every value's, so zero is the smaller operand of
 * any sum without a test for it. */
#define VMAF_MTL_SOFT_ZERO_EXP (-(1 << 20))

#define VMAF_MTL_SOFT_FRACTION_MASK (VMAF_MTL_SOFT_DOUBLE_TOP - 1u)
/* The bit pattern of a quiet NaN, for a caller that has to hand on "not a
 * number": no operation here forms or accepts one. */
#define VMAF_MTL_SOFT_QUIET_NAN_BITS (VMAF_MTL_U64(0x7FF8u) << 48)
#define VMAF_MTL_SOFT_EXPONENT_BIAS 1023
#define VMAF_MTL_SOFT_EXPONENT_FIELD_MASK 0x7FFu
#define VMAF_MTL_SOFT_MANTISSA_BITS 52
/* The guard bits a sum carries below the significand: the three of
 * vmaf_mtl_soft_round(). */
#define VMAF_MTL_SOFT_GUARD_BITS 3u
/* The highest bit of a significand with its guard bits. */
#define VMAF_MTL_SOFT_GUARDED_TOP_BIT 55u

VMAF_MTL_FUNC VmafMtlSoftSigned vmaf_mtl_signed_make(vmaf_mtl_u64 mant, vmaf_mtl_i32 exp,
                                                     bool negative)
{
    const bool zero = mant == 0u;
    // NOLINTNEXTLINE(modernize-use-designated-initializers): MSL has none, ADR-1498
    const VmafMtlSoftSigned value = {mant, zero ? VMAF_MTL_SOFT_ZERO_EXP : exp,
                                     (!zero && negative) ? 1u : 0u};
    return value;
}

/* fl64(value) for an unsigned integer: exact below 2^53, rounded above. */
VMAF_MTL_FUNC VmafMtlSoftSigned vmaf_mtl_signed_from_u64(vmaf_mtl_u64 value)
{
    const vmaf_mtl_u32 lead = VMAF_MTL_CLZ64(value | VMAF_MTL_U64(1));
    /* The highest set bit is bit 63 - lead; the significand's is bit 52. */
    const bool wide = lead < 11u;
    const vmaf_mtl_u32 drop = wide ? 11u - lead : 0u;
    const vmaf_mtl_u32 lift = wide ? 0u : lead - 11u;
    const vmaf_mtl_u32 half_bit = wide ? drop - 1u : 0u;
    const bool halfway = wide && ((value >> half_bit) & 1u) != 0u;
    const bool sticky = wide && (value & ((VMAF_MTL_U64(1) << half_bit) - 1u)) != 0u;
    vmaf_mtl_u64 kept = wide ? value >> drop : value << lift;
    if (halfway && (sticky || (kept & 1u) != 0u)) {
        kept += 1u;
    }
    vmaf_mtl_i32 exp = wide ? (vmaf_mtl_i32)drop : -(vmaf_mtl_i32)lift;
    if (kept == (VMAF_MTL_SOFT_DOUBLE_TOP << 1)) {
        kept = VMAF_MTL_SOFT_DOUBLE_TOP;
        exp += 1;
    }
    return vmaf_mtl_signed_make(value == 0u ? VMAF_MTL_U64(0) : kept, exp, false);
}

/* An unsigned integer below 2^53 as the fp64 value it is: no rounding. */
VMAF_MTL_FUNC VmafMtlSoftSigned vmaf_mtl_signed_from_exact(vmaf_mtl_u64 value)
{
    const vmaf_mtl_u32 lift = VMAF_MTL_CLZ64(value | VMAF_MTL_U64(1)) - 11u;
    // NOLINTNEXTLINE(clang-analyzer-core.BitwiseShift): value < 2^53, so clz64 is 11..63 and lift 0..52, ADR-1498
    return vmaf_mtl_signed_make(value << lift, -(vmaf_mtl_i32)lift, false);
}

/* A finite fp32 value as the fp64 value it is: exact, subnormals included. */
VMAF_MTL_FUNC VmafMtlSoftSigned vmaf_mtl_signed_from_float(float x)
{
    const vmaf_mtl_u32 bits = VMAF_MTL_F2U(x);
    const vmaf_mtl_u32 field = (bits >> 23) & 0xFFu;
    const vmaf_mtl_u64 fraction = bits & 0x007FFFFFu;
    /* A normal value is (2^23 + fraction) * 2^(field - 150), a subnormal one
     * fraction * 2^-149. */
    const vmaf_mtl_u64 integer = field == 0u ? fraction : (fraction | VMAF_MTL_U64(0x00800000u));
    const vmaf_mtl_i32 scale = field == 0u ? -149 : (vmaf_mtl_i32)field - 150;
    const VmafMtlSoftSigned magnitude = vmaf_mtl_signed_from_exact(integer);
    return vmaf_mtl_signed_make(magnitude.mant, magnitude.exp + scale, (bits >> 31) != 0u);
}

/* The value of an fp64 bit pattern: a normal value or zero. */
VMAF_MTL_FUNC VmafMtlSoftSigned vmaf_mtl_signed_from_bits(vmaf_mtl_u64 bits)
{
    const vmaf_mtl_u32 field =
        (vmaf_mtl_u32)(bits >> VMAF_MTL_SOFT_MANTISSA_BITS) & VMAF_MTL_SOFT_EXPONENT_FIELD_MASK;
    const vmaf_mtl_u64 mant = field == 0u ?
                                  VMAF_MTL_U64(0) :
                                  (bits & VMAF_MTL_SOFT_FRACTION_MASK) | VMAF_MTL_SOFT_DOUBLE_TOP;
    return vmaf_mtl_signed_make(
        mant, (vmaf_mtl_i32)field - VMAF_MTL_SOFT_EXPONENT_BIAS - VMAF_MTL_SOFT_MANTISSA_BITS,
        (bits >> 63) != 0u);
}

/* The fp64 bit pattern of a value. */
VMAF_MTL_FUNC vmaf_mtl_u64 vmaf_mtl_signed_bits(VmafMtlSoftSigned a)
{
    const vmaf_mtl_u64 field = VMAF_MTL_U64(
        (vmaf_mtl_u32)(a.exp + VMAF_MTL_SOFT_MANTISSA_BITS + VMAF_MTL_SOFT_EXPONENT_BIAS));
    const vmaf_mtl_u64 magnitude =
        (field << VMAF_MTL_SOFT_MANTISSA_BITS) | (a.mant & VMAF_MTL_SOFT_FRACTION_MASK);
    const vmaf_mtl_u64 sign = VMAF_MTL_U64(a.negative) << 63;
    return a.mant == 0u ? VMAF_MTL_U64(0) : sign | magnitude;
}

VMAF_MTL_FUNC VmafMtlSoftSigned vmaf_mtl_signed_negate(VmafMtlSoftSigned a)
{
    return vmaf_mtl_signed_make(a.mant, a.exp, a.negative == 0u);
}

/* |a|. */
VMAF_MTL_FUNC VmafMtlSoftSigned vmaf_mtl_signed_abs(VmafMtlSoftSigned a)
{
    return vmaf_mtl_signed_make(a.mant, a.exp, false);
}

/* 2 * a: exact. */
VMAF_MTL_FUNC VmafMtlSoftSigned vmaf_mtl_signed_twice(VmafMtlSoftSigned a)
{
    return vmaf_mtl_signed_make(a.mant, a.exp + 1, a.negative != 0u);
}

/* fl64(a + b).
 *
 * The smaller operand is aligned under the larger with three guard bits, the
 * bits shifted out kept as one sticky bit in the lowest place. For a sum that
 * is vmaf_mtl_soft_add(). For a difference: when the exponents are at most
 * one apart nothing is shifted out and the difference is exact, however many
 * leading bits cancel; when they are further apart the difference keeps at
 * least half of the larger operand, so it is normalised by one place at most
 * and the sticky bit stays below the rounding bit. */
VMAF_MTL_FUNC VmafMtlSoftSigned vmaf_mtl_signed_add(VmafMtlSoftSigned a, VmafMtlSoftSigned b)
{
    const bool swap = (b.exp > a.exp) || (b.exp == a.exp && b.mant > a.mant);
    const vmaf_mtl_u64 big_mant = swap ? b.mant : a.mant;
    const vmaf_mtl_u64 small_mant = swap ? a.mant : b.mant;
    const vmaf_mtl_i32 big_exp = swap ? b.exp : a.exp;
    const vmaf_mtl_i32 small_exp = swap ? a.exp : b.exp;
    const bool big_negative = (swap ? b.negative : a.negative) != 0u;
    const bool subtract = a.negative != b.negative;
    /* Beyond 59 places the smaller operand is below the last guard bit and
     * only sets the sticky bit; 59 gives the same result. */
    const vmaf_mtl_u32 distance = (vmaf_mtl_u32)(big_exp - small_exp);
    const vmaf_mtl_u32 shift = distance < 59u ? distance : 59u;
    const vmaf_mtl_u64 small_wide = small_mant << VMAF_MTL_SOFT_GUARD_BITS;
    const vmaf_mtl_u64 lost = small_wide & ((VMAF_MTL_U64(1) << shift) - 1u);
    const vmaf_mtl_u64 aligned =
        (small_wide >> shift) | (lost != 0u ? VMAF_MTL_U64(1) : VMAF_MTL_U64(0));
    const vmaf_mtl_u64 big_wide = big_mant << VMAF_MTL_SOFT_GUARD_BITS;

    vmaf_mtl_u64 sum = big_wide + aligned;
    vmaf_mtl_i32 sum_exp = big_exp - (vmaf_mtl_i32)VMAF_MTL_SOFT_GUARD_BITS;
    if (sum >= (VMAF_MTL_SOFT_DOUBLE_TOP << (VMAF_MTL_SOFT_GUARD_BITS + 1u))) {
        sum = (sum >> 1) | (sum & 1u);
        sum_exp += 1;
    }

    const vmaf_mtl_u64 difference = big_wide - aligned;
    const vmaf_mtl_u32 lead = VMAF_MTL_CLZ64(difference | VMAF_MTL_U64(1));
    const vmaf_mtl_u32 lift = lead - (63u - VMAF_MTL_SOFT_GUARDED_TOP_BIT);
    const vmaf_mtl_u64 difference_wide = difference << lift;
    const vmaf_mtl_i32 difference_exp =
        big_exp - (vmaf_mtl_i32)VMAF_MTL_SOFT_GUARD_BITS - (vmaf_mtl_i32)lift;

    const VmafMtlSoftDouble rounded =
        vmaf_mtl_soft_round(subtract ? difference_wide : sum, subtract ? difference_exp : sum_exp);
    const bool zero = subtract ? difference == 0u : big_mant == 0u;
    return vmaf_mtl_signed_make(zero ? VMAF_MTL_U64(0) : rounded.mant, rounded.exp, big_negative);
}

/* fl64(a - b). */
VMAF_MTL_FUNC VmafMtlSoftSigned vmaf_mtl_signed_sub(VmafMtlSoftSigned a, VmafMtlSoftSigned b)
{
    return vmaf_mtl_signed_add(a, vmaf_mtl_signed_negate(b));
}

/* fl64(a * b). */
VMAF_MTL_FUNC VmafMtlSoftSigned vmaf_mtl_signed_mul(VmafMtlSoftSigned a, VmafMtlSoftSigned b)
{
    const VmafMtlSoftDouble product =
        vmaf_mtl_soft_mul(vmaf_mtl_soft_make(a.mant, a.exp), vmaf_mtl_soft_make(b.mant, b.exp));
    const bool zero = a.mant == 0u || b.mant == 0u;
    return vmaf_mtl_signed_make(zero ? VMAF_MTL_U64(0) : product.mant, product.exp,
                                a.negative != b.negative);
}

/* One step of a long division in radix 2^19: the next 19 quotient bits of
 * rem / den and the remainder they leave, for rem < den and den in
 * [2^52, 2^53).
 *
 * The digit is estimated in fp32 from the top 24 bits of each operand, which
 * fp32 holds exactly. The estimate is within one of the digit: the truncated
 * operands move the quotient by less than 2^-22 and the fp32 division by less
 * than 2^-22 more, a quarter of a digit together, and the conversion to an
 * integer truncates. The remainder is then exact integer arithmetic: it lies
 * within three divisors of zero, so its low 64 bits are its value, and two
 * corrections each way put the digit right whichever way the device's
 * division rounds. */
// NOLINTNEXTLINE(modernize-use-using): C and MSL share this header, ADR-1498
typedef struct VmafMtlDivStep {
    vmaf_mtl_u64 rem;
    vmaf_mtl_u64 digit;
} VmafMtlDivStep;

#define VMAF_MTL_SOFT_DIGIT_BITS 19u
#define VMAF_MTL_SOFT_DIGIT_SCALE 524288.0f
/* The bits of a significand below its top 24. */
#define VMAF_MTL_SOFT_BELOW_FLOAT 29u

VMAF_MTL_FUNC VmafMtlDivStep vmaf_mtl_div_step(vmaf_mtl_u64 rem, vmaf_mtl_u64 den)
{
    const float estimate = (float)(vmaf_mtl_u32)(rem >> VMAF_MTL_SOFT_BELOW_FLOAT) /
                           (float)(vmaf_mtl_u32)(den >> VMAF_MTL_SOFT_BELOW_FLOAT);
    vmaf_mtl_u64 digit = VMAF_MTL_U64((vmaf_mtl_u32)(estimate * VMAF_MTL_SOFT_DIGIT_SCALE));
    vmaf_mtl_i64 left = (vmaf_mtl_i64)((rem << VMAF_MTL_SOFT_DIGIT_BITS) - digit * den);
    const vmaf_mtl_i64 divisor = (vmaf_mtl_i64)den;
    for (int correction = 0; correction < 2; correction++) {
        const bool below = left < 0;
        left += below ? divisor : VMAF_MTL_I64(0);
        digit -= below ? VMAF_MTL_U64(1) : VMAF_MTL_U64(0);
    }
    for (int correction = 0; correction < 2; correction++) {
        const bool above = left >= divisor;
        left -= above ? divisor : VMAF_MTL_I64(0);
        digit += above ? VMAF_MTL_U64(1) : VMAF_MTL_U64(0);
    }
    // NOLINTNEXTLINE(modernize-use-designated-initializers): MSL has none, ADR-1498
    const VmafMtlDivStep next = {(vmaf_mtl_u64)left, digit};
    return next;
}

/* fl64(a / b) for positive a and b: vmaf_mtl_soft_div()'s result from three
 * radix-2^19 steps instead of 56 one-bit steps. The quotient's first bit and
 * 57 more are formed; the two lowest and the remainder are the sticky bit. */
VMAF_MTL_FUNC VmafMtlSoftDouble vmaf_mtl_soft_div_digits(VmafMtlSoftDouble a, VmafMtlSoftDouble b)
{
    vmaf_mtl_u64 rem = a.mant;
    vmaf_mtl_i32 exp = a.exp - b.exp - 55;
    if (rem < b.mant) {
        rem <<= 1;
        exp -= 1;
    }
    /* rem is in [b.mant, 2 * b.mant): the first quotient bit is one. */
    rem -= b.mant;
    vmaf_mtl_u64 quot = 1u;
    for (int step = 0; step < 3; step++) {
        const VmafMtlDivStep next = vmaf_mtl_div_step(rem, b.mant);
        quot = (quot << VMAF_MTL_SOFT_DIGIT_BITS) | next.digit;
        rem = next.rem;
    }
    const bool sticky = (quot & 3u) != 0u || rem != 0u;
    return vmaf_mtl_soft_round((quot >> 2) | (sticky ? VMAF_MTL_U64(1) : VMAF_MTL_U64(0)), exp);
}

/* fl64(a / b); b is not zero. */
VMAF_MTL_FUNC VmafMtlSoftSigned vmaf_mtl_signed_div(VmafMtlSoftSigned a, VmafMtlSoftSigned b)
{
    const VmafMtlSoftDouble quotient = vmaf_mtl_soft_div_digits(vmaf_mtl_soft_make(a.mant, a.exp),
                                                                vmaf_mtl_soft_make(b.mant, b.exp));
    return vmaf_mtl_signed_make(a.mant == 0u ? VMAF_MTL_U64(0) : quotient.mant, quotient.exp,
                                a.negative != b.negative);
}

#endif /* VMAF_FEATURE_METAL_METAL_SOFT_SIGNED_H_ */
