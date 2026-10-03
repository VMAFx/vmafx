/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-1498: the fp64 operations the exact Metal twins run in 64-bit integers
 * (core/src/feature/metal/metal_soft_double.h and metal_soft_signed.h, the
 * Metal spelling of the SYCL twins' sycl_soft_double.h / sycl_soft_signed.h),
 * compiled on the host and held against the host's own IEEE-754 binary64
 * arithmetic, every result compared bit for bit.
 *
 * Each operation runs on at least 10^6 operands from a fixed SplitMix64
 * sequence (the same on every host) that cycles through the cases the
 * headers distinguish: random significands over 600 binades, exponents at
 * most one apart (an exact difference), a smaller operand 50 to 70 binades
 * below the larger (only the sticky bit), exact ties and near-ties of the
 * rounding, integers, powers of two, products that are exact ties (two odd
 * 27-bit significands), quotients next to a rounding midpoint, sums and
 * products that round up to a power of two (the carry out of the
 * significand), equal and adjacent operands, cancellations to a few bits,
 * zeros; and on a table of edge values (the smallest and largest normals,
 * 1 + 2^-52, 2^53 + 1, ...) taken pairwise. The fp32 conversions run over
 * normal fp32 values and every subnormal one, values just below a power of
 * two, ties at the subnormal boundary, underflow to zero and overflow to
 * infinity. The 128-bit helpers are held against bit-by-bit references.
 *
 * Two properties of the headers are part of what is checked: every result
 * is normalised (a significand in [2^52, 2^53)), and a zero has no sign (a
 * signed result that is zero must be +0.0 with VMAF_MTL_SOFT_ZERO_EXP).
 * The headers form no fp64 subnormal, infinity or NaN, so operands whose
 * fp64 result would be one are not used.
 *
 * What this cannot show: that the same source compiles as Metal Shading
 * Language (the macOS job compiles a header once a kernel includes it) and
 * returns the same bits on an Apple GPU (the tester's device run of the
 * twins built on it), and vmaf_mtl_div_step()'s corrections under a division
 * that rounds other than to nearest (the host's fp32 division is correctly
 * rounded; the corrections cover one step either way).
 */

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "test.h"

#include "feature/metal/metal_soft_signed.h"

namespace
{

constexpr unsigned kSamples = 1000000u;
constexpr unsigned kReport = 5u; /* mismatches printed per operation */
constexpr uint64_t kTop = UINT64_C(1) << 52;
constexpr uint64_t kFraction = kTop - 1u;
/* A pattern no operation returns: marks a malformed result. */
constexpr uint64_t kMalformed = UINT64_C(0x7FF0DEADBEEF0001);

uint64_t bits_of(double v)
{
    uint64_t b = 0u;
    std::memcpy(&b, &v, sizeof(b));
    return b;
}

double value_of(uint64_t b)
{
    double v = 0.0;
    std::memcpy(&v, &b, sizeof(v));
    return v;
}

uint32_t fbits_of(float v)
{
    uint32_t b = 0u;
    std::memcpy(&b, &v, sizeof(b));
    return b;
}

float fvalue_of(uint32_t b)
{
    float v = 0.0f;
    std::memcpy(&v, &b, sizeof(v));
    return v;
}

/* SplitMix64 of a counter: a fixed sequence, the same on every host. */
class Sequence
{
  public:
    explicit Sequence(uint64_t seed) : state_(seed)
    {
    }
    uint64_t next()
    {
        state_ += UINT64_C(0x9E3779B97F4A7C15);
        uint64_t z = state_;
        z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
        z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
        return z ^ (z >> 31);
    }
    /* In [0, n). */
    int below(uint64_t n)
    {
        return (int)(next() % n);
    }
    bool coin()
    {
        return (next() & 1u) != 0u;
    }

  private:
    uint64_t state_;
};

/* ------------------------------------------------------------------ */
/* Independent encodings                                               */
/* ------------------------------------------------------------------ */

/* A positive normal double as a VmafMtlSoftDouble. */
VmafMtlSoftDouble soft_of(double v)
{
    const uint64_t b = bits_of(v);
    VmafMtlSoftDouble s;
    s.mant = (b & kFraction) | kTop;
    s.exp = (int32_t)((b >> 52) & 0x7FFu) - 1075;
    return s;
}

/* The bit pattern of a VmafMtlSoftDouble whose value is a normal double, or
 * kMalformed if its significand is not normalised. */
uint64_t bits_of_soft(VmafMtlSoftDouble s)
{
    if ((s.mant >> 52) != 1u)
        return kMalformed;
    return bits_of(std::ldexp((double)s.mant, s.exp));
}

/* A normal double or a zero (of either sign) as a VmafMtlSoftSigned. */
VmafMtlSoftSigned signed_of(double v)
{
    VmafMtlSoftSigned s;
    s.mant = 0u;
    s.exp = VMAF_MTL_SOFT_ZERO_EXP;
    s.negative = 0u;
    if (v == 0.0)
        return s;
    const VmafMtlSoftDouble magnitude = soft_of(std::fabs(v));
    s.mant = magnitude.mant;
    s.exp = magnitude.exp;
    s.negative = std::signbit(v) ? 1u : 0u;
    return s;
}

/* The bit pattern of a VmafMtlSoftSigned, or kMalformed if it breaks the
 * header's invariants (a zero is mant 0, VMAF_MTL_SOFT_ZERO_EXP, positive). */
uint64_t bits_of_signed(VmafMtlSoftSigned s)
{
    if (s.mant == 0u) {
        const bool canonical = s.exp == VMAF_MTL_SOFT_ZERO_EXP && s.negative == 0u;
        return canonical ? UINT64_C(0) : kMalformed;
    }
    if ((s.mant >> 52) != 1u || s.negative > 1u)
        return kMalformed;
    const double magnitude = std::ldexp((double)s.mant, s.exp);
    return bits_of(s.negative != 0u ? -magnitude : magnitude);
}

/* The bits the header must return for an fp64 result: a zero has no sign. */
uint64_t expected_bits(double r)
{
    return r == 0.0 ? UINT64_C(0) : bits_of(r);
}

/* An fp64 result the headers can form: a normal value or zero. */
bool representable(double r)
{
    return r == 0.0 || std::isnormal(r);
}

bool same_soft(VmafMtlSoftDouble a, VmafMtlSoftDouble b)
{
    return a.mant == b.mant && a.exp == b.exp;
}

/* ------------------------------------------------------------------ */
/* Tally                                                               */
/* ------------------------------------------------------------------ */

class Tally
{
  public:
    explicit Tally(const char *name) : name_(name)
    {
    }
    void compare(uint64_t got, uint64_t want, double a, double b)
    {
        checked_++;
        if (got == want)
            return;
        if (wrong_ < kReport) {
            (void)fprintf(stderr, "\n%s(%a, %a): got 0x%016llx (%a), want 0x%016llx (%a)", name_, a,
                          b, (unsigned long long)got, value_of(got), (unsigned long long)want,
                          value_of(want));
        }
        wrong_++;
    }
    /* Passed, and covered at least `minimum` operand sets. */
    bool passed(unsigned minimum) const
    {
        if (wrong_ != 0u) {
            (void)fprintf(stderr, "\n%s: %u of %u results differ\n", name_, wrong_, checked_);
        }
        if (checked_ < minimum) {
            (void)fprintf(stderr, "\n%s: only %u operand sets\n", name_, checked_);
        }
        return wrong_ == 0u && checked_ >= minimum;
    }
    unsigned checked() const
    {
        return checked_;
    }

  private:
    const char *name_;
    unsigned checked_ = 0u;
    unsigned wrong_ = 0u;
};

/* ------------------------------------------------------------------ */
/* Operands                                                            */
/* ------------------------------------------------------------------ */

/* A positive normal double with a random significand and a binary exponent
 * in [low, low + span). */
double random_positive(Sequence &s, int low, int span)
{
    const uint64_t mant = (s.next() >> 12) | kTop;
    const int e = low + s.below((uint64_t)span);
    return std::ldexp((double)mant, e - 52);
}

/* Half a unit in the last place of a positive normal double. */
double half_ulp(double a)
{
    return std::ldexp(1.0, std::ilogb(a) - 53);
}

/* An odd 27-bit integer with its top bit set. */
double odd27(Sequence &s)
{
    return (double)((s.next() >> 37) | (UINT64_C(1) << 26) | 1u);
}

enum PairKind {
    PAIR_RANDOM,
    PAIR_NEAR_EXPONENT, /* exponents at most one apart */
    PAIR_FAR_BELOW,     /* the smaller 50 to 70 binades below */
    PAIR_TIE,           /* b an odd multiple of half an ulp of a */
    PAIR_NEAR_TIE,      /* b next to half an ulp of a */
    PAIR_INTEGERS,
    PAIR_POWERS,
    PAIR_PRODUCT_TIE,   /* a * b an exact tie */
    PAIR_QUOTIENT_MID,  /* a / b next to a rounding midpoint */
    PAIR_SUM_CARRY,     /* a + b rounds up to a power of two */
    PAIR_PRODUCT_CARRY, /* a * b rounds up to a power of two */
    PAIR_ADJACENT,      /* equal, or one ulp apart */
    PAIR_KINDS,
};

/* A sum or a product that rounds up to a power of two: the carry out of the
 * significand. (A quotient never does: a / b is at least 2^-53 of itself
 * from any power of two it is not equal to.) */
void carry_pair(Sequence &s, unsigned kind, double &a, double &b)
{
    if (kind == PAIR_SUM_CARRY) {
        /* The largest double below 2^e plus 1 to 1.75 of its half ulp. */
        a = std::nextafter(std::ldexp(1.0, s.below(400) - 200), 0.0);
        b = half_ulp(a) * (1.0 + (double)s.below(4) / 4.0);
        return;
    }
    /* A * Q at most 2^51 below 2^105, A and Q in [2^52, 2^53): their
     * product's significand rounds to 2^53. */
    for (int attempt = 0; attempt < 32; attempt++) {
        const double big_a = (double)((s.next() >> 11) | kTop);
        const double q = 0x1p105 / big_a;
        const double below = std::fma(big_a, q, -0x1p105); /* exact */
        if (below < 0.0 && below >= -0x1p51) {
            a = std::ldexp(big_a, s.below(400) - 252);
            b = std::ldexp(q, s.below(400) - 252);
            return;
        }
    }
}

void special_pair(Sequence &s, unsigned kind, double &a, double &b)
{
    const int scale = s.below(400) - 200;
    if (kind == PAIR_PRODUCT_TIE) {
        /* Two odd 27-bit significands: a 54-bit odd product is a tie. */
        a = std::ldexp(odd27(s), scale);
        b = std::ldexp(odd27(s), s.below(400) - 200);
        return;
    }
    /* m = (2^53 + 2r + 1) / 2^53 is a midpoint; a = fl(m * b). */
    const double m_hi = 1.0 + std::ldexp((double)(s.next() >> 12), -52);
    const double base = random_positive(s, 0, 1);
    a = std::ldexp(std::fma(m_hi, base, std::ldexp(base, -53)), scale);
    b = std::ldexp(base, scale + s.below(40) - 20);
}

/* One pair of positive normal operands of the given kind. */
void positive_pair(Sequence &s, unsigned kind, double &a, double &b)
{
    a = random_positive(s, -300, 600);
    b = random_positive(s, -300, 600);
    switch (kind) {
    case PAIR_NEAR_EXPONENT:
        b = std::ldexp(b, std::ilogb(a) - std::ilogb(b) + s.below(3) - 1);
        break;
    case PAIR_FAR_BELOW:
        b = std::ldexp(b, std::ilogb(a) - std::ilogb(b) - 50 - s.below(21));
        break;
    case PAIR_TIE:
        b = half_ulp(a) * (double)(1 + 2 * s.below(1u << 20));
        break;
    case PAIR_NEAR_TIE:
        b = half_ulp(a) * (1.0 + std::ldexp(s.coin() ? 1.0 : -1.0, -1 - s.below(50)));
        break;
    case PAIR_INTEGERS:
        a = (double)(((s.next() >> 11) >> s.below(53)) + 1u);
        b = (double)(((s.next() >> 11) >> s.below(53)) + 1u);
        break;
    case PAIR_POWERS:
        a = std::ldexp(1.0, s.below(600) - 300);
        b = std::ldexp(1.0, s.below(600) - 300);
        break;
    case PAIR_PRODUCT_TIE:
    case PAIR_QUOTIENT_MID:
        special_pair(s, kind, a, b);
        break;
    case PAIR_SUM_CARRY:
    case PAIR_PRODUCT_CARRY:
        carry_pair(s, kind, a, b);
        break;
    case PAIR_ADJACENT:
        b = s.coin() ? a : std::nextafter(a, s.coin() ? INFINITY : 0.0);
        break;
    default:
        break;
    }
}

/* One pair of signed operands: a positive pair with random signs, a pair
 * that cancels to a few bits, or one with a zero. */
void signed_pair(Sequence &s, unsigned index, double &a, double &b)
{
    const unsigned kind = index % (PAIR_KINDS + 2u);
    if (kind < PAIR_KINDS) {
        positive_pair(s, kind, a, b);
    } else if (kind == PAIR_KINDS) {
        a = random_positive(s, -300, 600);
        const uint64_t steps = 1u + (uint64_t)s.below(4);
        b = value_of(bits_of(a) + (s.coin() ? steps : (uint64_t)0 - steps));
    } else {
        positive_pair(s, (unsigned)s.below(PAIR_KINDS), a, b);
        a = s.coin() ? a : (s.coin() ? 0.0 : -0.0);
        b = s.coin() ? b : (s.coin() ? 0.0 : -0.0);
    }
    a = s.coin() ? -a : a;
    b = s.coin() ? -b : b;
}

/* Edge values: the normal range's ends, 1 and its neighbours, the integers
 * around 2^53, powers of two, values without a short binary form. */
const double kEdges[] = {
    DBL_MIN,
    DBL_MIN * (1.0 + DBL_EPSILON),
    2.0 * DBL_MIN,
    0x1p-600,
    0x1p-53,
    0.1,
    1.0 / 3.0,
    0.5,
    1.0 - DBL_EPSILON / 2.0,
    1.0,
    1.0 + DBL_EPSILON,
    1.5,
    2.0 - DBL_EPSILON,
    2.0,
    3.0,
    4503599627370497.0, /* 2^52 + 1 */
    9007199254740991.0, /* 2^53 - 1 */
    9007199254740992.0, /* 2^53 */
    9007199254740994.0, /* 2^53 + 2 */
    0x1p600,
    0x1p1022,
    DBL_MAX / 2.0,
    DBL_MAX,
};
constexpr unsigned kEdgeCount = sizeof(kEdges) / sizeof(kEdges[0]);

/* ------------------------------------------------------------------ */
/* Positive operations                                                 */
/* ------------------------------------------------------------------ */

typedef VmafMtlSoftDouble (*SoftBinary)(VmafMtlSoftDouble, VmafMtlSoftDouble);
typedef double (*NativeBinary)(double, double);

double native_add(double a, double b)
{
    return a + b;
}

double native_mul(double a, double b)
{
    return a * b;
}

double native_div(double a, double b)
{
    return a / b;
}

double native_sub(double a, double b)
{
    return a - b;
}

void compare_positive(Tally &tally, SoftBinary soft, NativeBinary native, double a, double b)
{
    const double r = native(a, b);
    if (!std::isnormal(r))
        return;
    tally.compare(bits_of_soft(soft(soft_of(a), soft_of(b))), bits_of(r), a, b);
}

bool positive_binary_passes(const char *name, SoftBinary soft, NativeBinary native, uint64_t seed)
{
    Tally tally(name);
    Sequence s(seed);
    for (unsigned i = 0u; i < kSamples; i++) {
        double a = 0.0;
        double b = 0.0;
        positive_pair(s, i % PAIR_KINDS, a, b);
        compare_positive(tally, soft, native, a, b);
    }
    for (unsigned i = 0u; i < kEdgeCount; i++) {
        for (unsigned j = 0u; j < kEdgeCount; j++) {
            compare_positive(tally, soft, native, kEdges[i], kEdges[j]);
        }
    }
    return tally.passed(kSamples);
}

mu_message_t test_soft_add_is_the_fp64_sum()
{
    mu_assert("vmaf_mtl_soft_add() is not fl64(a + b)",
              positive_binary_passes("soft_add", vmaf_mtl_soft_add, native_add, 1u));
    return nullptr;
}

mu_message_t test_soft_mul_is_the_fp64_product()
{
    mu_assert("vmaf_mtl_soft_mul() is not fl64(a * b)",
              positive_binary_passes("soft_mul", vmaf_mtl_soft_mul, native_mul, 2u));
    return nullptr;
}

mu_message_t test_soft_div_is_the_fp64_quotient()
{
    mu_assert("vmaf_mtl_soft_div() is not fl64(a / b)",
              positive_binary_passes("soft_div", vmaf_mtl_soft_div, native_div, 3u));
    mu_assert("vmaf_mtl_soft_div_digits() is not fl64(a / b)",
              positive_binary_passes("soft_div_digits", vmaf_mtl_soft_div_digits, native_div, 4u));
    return nullptr;
}

/* The two divisions agree on every operand, not only on the rounded value:
 * soft_div_digits() is soft_div() in three steps. */
mu_message_t test_soft_div_digits_is_soft_div()
{
    Sequence s(5u);
    unsigned wrong = 0u;
    for (unsigned i = 0u; i < kSamples; i++) {
        double a = 0.0;
        double b = 0.0;
        positive_pair(s, i % PAIR_KINDS, a, b);
        const VmafMtlSoftDouble x = soft_of(a);
        const VmafMtlSoftDouble y = soft_of(b);
        wrong += same_soft(vmaf_mtl_soft_div(x, y), vmaf_mtl_soft_div_digits(x, y)) ? 0u : 1u;
    }
    mu_assert("vmaf_mtl_soft_div_digits() and vmaf_mtl_soft_div() differ", wrong == 0u);
    return nullptr;
}

mu_message_t test_soft_less_is_the_fp64_comparison()
{
    Tally tally("soft_less");
    Sequence s(6u);
    for (unsigned i = 0u; i < kSamples; i++) {
        double a = 0.0;
        double b = 0.0;
        positive_pair(s, i % PAIR_KINDS, a, b);
        const bool got = vmaf_mtl_soft_less(soft_of(a), soft_of(b));
        tally.compare(got ? 1u : 0u, a < b ? 1u : 0u, a, b);
    }
    for (unsigned i = 0u; i < kEdgeCount; i++) {
        for (unsigned j = 0u; j < kEdgeCount; j++) {
            const bool got = vmaf_mtl_soft_less(soft_of(kEdges[i]), soft_of(kEdges[j]));
            tally.compare(got ? 1u : 0u, kEdges[i] < kEdges[j] ? 1u : 0u, kEdges[i], kEdges[j]);
        }
    }
    mu_assert("vmaf_mtl_soft_less() is not a < b", tally.passed(kSamples));
    return nullptr;
}

/* (int64_t)value for 0 < value < 2^63: random values, integers, values just
 * below an integer, 2^63's neighbour. */
mu_message_t test_soft_trunc_is_the_conversion()
{
    Tally tally("soft_trunc");
    Sequence s(7u);
    const double edges[] = {DBL_MIN,      0.5,    1.0 - DBL_EPSILON / 2.0, 1.0, 1.5, 0x1p52,
                            0x1p53 - 1.0, 0x1p62, 0x1p63 - 1024.0};
    for (double v : edges) {
        tally.compare((uint64_t)vmaf_mtl_soft_trunc(soft_of(v)), (uint64_t)(int64_t)v, v, 0.0);
    }
    for (unsigned i = 0u; i < kSamples; i++) {
        double v = random_positive(s, -12, 75);
        if ((i % 3u) == 1u)
            v = std::floor(v) + 1.0;
        if ((i % 3u) == 2u)
            v = std::nextafter(std::floor(v) + 1.0, 0.0);
        tally.compare((uint64_t)vmaf_mtl_soft_trunc(soft_of(v)), (uint64_t)(int64_t)v, v, 0.0);
    }
    mu_assert("vmaf_mtl_soft_trunc() is not (int64_t)value", tally.passed(kSamples));
    return nullptr;
}

/* trunc(fl64(a - t)) for an integer a below 2^31 and 2^-31 <= t < a: random
 * t, t next to an integer, t so small that a - t rounds back to a. */
double sub_trunc_operand(Sequence &s, unsigned kind, uint32_t a)
{
    const int top = std::ilogb((double)a);
    double t = random_positive(s, -31, top + 32);
    if (kind == 1u)
        t = (double)(1 + s.below(a)) + std::ldexp(s.coin() ? 1.0 : -1.0, -1 - s.below(40));
    if (kind == 2u)
        t = std::ldexp(1.0 + std::ldexp((double)(s.next() >> 12), -52), -31 + s.below(8));
    return t;
}

mu_message_t test_soft_sub_trunc_is_the_truncated_difference()
{
    Tally tally("soft_sub_trunc");
    Sequence s(8u);
    for (unsigned i = 0u; tally.checked() < kSamples && i < 2u * kSamples; i++) {
        const uint32_t a = 1u + (uint32_t)s.below(0x7FFFFFFFu);
        const double t = sub_trunc_operand(s, i % 3u, a);
        if (!(t >= 0x1p-31 && t < (double)a))
            continue;
        const uint32_t got = vmaf_mtl_soft_sub_trunc(a, soft_of(t));
        tally.compare(got, (uint32_t)((double)a - t), (double)a, t);
    }
    mu_assert("vmaf_mtl_soft_sub_trunc() is not trunc(fl64(a - t))", tally.passed(kSamples));
    return nullptr;
}

/* ------------------------------------------------------------------ */
/* fp32 conversions                                                    */
/* ------------------------------------------------------------------ */

mu_message_t test_soft_from_float_is_exact()
{
    Sequence s(9u);
    unsigned wrong = 0u;
    unsigned wrong_any = 0u;
    const uint32_t edges[] = {0x00800000u, 0x00800001u, 0x3F800000u, 0x3F800001u, 0x7F7FFFFFu};
    for (unsigned i = 0u; i < kSamples + 5u; i++) {
        const uint32_t bits = i < 5u ? edges[i] : 0x00800000u + (uint32_t)s.below(0x7F000000u);
        const float x = fvalue_of(bits);
        const VmafMtlSoftDouble want = soft_of((double)x);
        wrong += same_soft(vmaf_mtl_soft_from_float(x), want) ? 0u : 1u;
        wrong_any += same_soft(vmaf_mtl_soft_from_float_any(x), want) ? 0u : 1u;
    }
    mu_assert("vmaf_mtl_soft_from_float() is not the float's value", wrong == 0u);
    mu_assert("vmaf_mtl_soft_from_float_any() is not a normal float's value", wrong_any == 0u);
    return nullptr;
}

/* Subnormal fp32 values: every one of the 2^23 - 1 is checked. */
mu_message_t test_soft_from_float_any_takes_subnormals()
{
    unsigned wrong = 0u;
    for (uint32_t bits = 1u; bits < 0x00800000u; bits++) {
        const float x = fvalue_of(bits);
        wrong += same_soft(vmaf_mtl_soft_from_float_any(x), soft_of((double)x)) ? 0u : 1u;
    }
    mu_assert("vmaf_mtl_soft_from_float_any() is not a subnormal float's value", wrong == 0u);
    return nullptr;
}

mu_message_t test_soft_from_u32_is_exact()
{
    Sequence s(10u);
    unsigned wrong = 0u;
    const uint32_t edges[] = {1u, 2u, 3u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu};
    for (unsigned i = 0u; i < kSamples + 6u; i++) {
        const uint32_t x = i < 6u ? edges[i] : 1u + (uint32_t)(s.next() >> (32 + s.below(32)));
        wrong += same_soft(vmaf_mtl_soft_from_u32(x), soft_of((double)x)) ? 0u : 1u;
    }
    mu_assert("vmaf_mtl_soft_from_u32() is not the integer's value", wrong == 0u);
    return nullptr;
}

/* (float)d with the IEEE-754 overflow to infinity spelled out: past
 * FLT_MAX + half its ulp a conversion is undefined in C++. */
float native_to_float(double d)
{
    return d >= 0x1p128 - 0x1p103 ? INFINITY : (float)d;
}

/* A positive double for an fp32 conversion: random over the range given,
 * just below a power of two (it rounds up to it: the carry), an exact
 * midpoint between two fp32 values, or one ulp beside it. */
double to_float_operand(Sequence &s, unsigned kind, int low, int span)
{
    const double v = random_positive(s, low, span);
    if (kind == 0u)
        return v;
    if (kind == 3u)
        return std::ldexp(1.0 - std::ldexp(1.0, -25 - s.below(28)), std::ilogb(v) + 1);
    const float f = native_to_float(v);
    const double up = (double)std::nextafter(f, INFINITY);
    const double mid = std::isinf(up) ? (double)FLT_MAX + 0x1p103 : ((double)f + up) / 2.0;
    if (kind == 1u)
        return mid;
    return std::nextafter(mid, s.coin() ? INFINITY : 0.0);
}

mu_message_t test_soft_to_float_rounds_to_nearest_even()
{
    Tally tally("soft_to_float");
    Sequence s(11u);
    /* The documented domain [1, 2^100) and the whole normal fp32 range, where
     * the same arithmetic holds. */
    for (unsigned i = 0u; i < kSamples; i++) {
        const bool documented = (i % 2u) == 0u;
        const double d =
            to_float_operand(s, (i / 2u) % 4u, documented ? 0 : -126, documented ? 100 : 254);
        const float got = vmaf_mtl_soft_to_float(soft_of(d));
        tally.compare(fbits_of(got), fbits_of(native_to_float(d)), d, 0.0);
    }
    mu_assert("vmaf_mtl_soft_to_float() is not (float)value", tally.passed(kSamples));
    return nullptr;
}

/* Every regime of (float)d: zero, the subnormal range and its ties, the
 * normal range, overflow to infinity. */
const double kToFloatEdges[] = {
    0x1p-200,
    0x1p-150 * (1.0 - DBL_EPSILON),
    0x1p-150,
    0x1p-150 * (1.0 + DBL_EPSILON),
    0x1p-149,
    0x1.8p-149,
    0x1p-148,
    0x1.fffffdp-127,
    0x1.fffffep-127,
    0x1.ffffffp-127,
    0x1p-126,
    1.0,
    (double)FLT_MAX,
    (double)FLT_MAX + 0x1p103 - 0x1p75,
    (double)FLT_MAX + 0x1p103,
    0x1p128,
    0x1p300,
};

mu_message_t test_soft_to_float_any_covers_every_regime()
{
    Tally tally("soft_to_float_any");
    Sequence s(12u);
    for (double d : kToFloatEdges) {
        tally.compare(fbits_of(vmaf_mtl_soft_to_float_any(soft_of(d))),
                      fbits_of(native_to_float(d)), d, 0.0);
    }
    for (unsigned i = 0u; i < kSamples; i++) {
        double d = to_float_operand(s, i % 4u, -200, 340);
        if ((i % 7u) == 0u) /* (k + 1/2) * 2^-149: a subnormal tie */
            d = std::ldexp((double)(2 * s.below(1u << 24) + 1), -150);
        const float got = vmaf_mtl_soft_to_float_any(soft_of(d));
        tally.compare(fbits_of(got), fbits_of(native_to_float(d)), d, 0.0);
    }
    mu_assert("vmaf_mtl_soft_to_float_any() is not (float)value", tally.passed(kSamples));
    return nullptr;
}

/* ------------------------------------------------------------------ */
/* Signed operations                                                   */
/* ------------------------------------------------------------------ */

typedef VmafMtlSoftSigned (*SignedBinary)(VmafMtlSoftSigned, VmafMtlSoftSigned);

void compare_signed(Tally &tally, SignedBinary soft, NativeBinary native, double a, double b)
{
    if (native == native_div && b == 0.0)
        return;
    const double r = native(a, b);
    if (!representable(r))
        return;
    /* A product or quotient of non-zero values that rounds to zero has
     * underflowed: outside the headers' range, like a subnormal result. */
    const bool nonzero = native == native_div ? a != 0.0 : (a != 0.0 && b != 0.0);
    if (r == 0.0 && nonzero && (native == native_mul || native == native_div))
        return;
    tally.compare(bits_of_signed(soft(signed_of(a), signed_of(b))), expected_bits(r), a, b);
}

bool signed_binary_passes(const char *name, SignedBinary soft, NativeBinary native, uint64_t seed)
{
    Tally tally(name);
    Sequence s(seed);
    for (unsigned i = 0u; i < kSamples; i++) {
        double a = 0.0;
        double b = 0.0;
        signed_pair(s, i, a, b);
        if (native == native_div && b == 0.0)
            b = 3.0;
        compare_signed(tally, soft, native, a, b);
    }
    for (unsigned i = 0u; i < 2u * kEdgeCount + 2u; i++) {
        for (unsigned j = 0u; j < 2u * kEdgeCount + 2u; j++) {
            const double a = i < 2u * kEdgeCount ? kEdges[i / 2u] * (i % 2u ? -1.0 : 1.0) :
                                                   (i % 2u ? -0.0 : 0.0);
            const double b = j < 2u * kEdgeCount ? kEdges[j / 2u] * (j % 2u ? -1.0 : 1.0) :
                                                   (j % 2u ? -0.0 : 0.0);
            compare_signed(tally, soft, native, a, b);
        }
    }
    return tally.passed(kSamples);
}

mu_message_t test_signed_add_sub_are_the_fp64_operations()
{
    mu_assert("vmaf_mtl_signed_add() is not fl64(a + b)",
              signed_binary_passes("signed_add", vmaf_mtl_signed_add, native_add, 21u));
    mu_assert("vmaf_mtl_signed_sub() is not fl64(a - b)",
              signed_binary_passes("signed_sub", vmaf_mtl_signed_sub, native_sub, 22u));
    return nullptr;
}

mu_message_t test_signed_mul_div_are_the_fp64_operations()
{
    mu_assert("vmaf_mtl_signed_mul() is not fl64(a * b)",
              signed_binary_passes("signed_mul", vmaf_mtl_signed_mul, native_mul, 23u));
    mu_assert("vmaf_mtl_signed_div() is not fl64(a / b)",
              signed_binary_passes("signed_div", vmaf_mtl_signed_div, native_div, 24u));
    return nullptr;
}

/* A random normal double or zero of either sign. */
double random_signed(Sequence &s, unsigned i)
{
    if ((i % 50u) == 0u)
        return s.coin() ? 0.0 : -0.0;
    const double v = random_positive(s, -1022, 2045);
    return s.coin() ? -v : v;
}

mu_message_t test_signed_bits_round_trip()
{
    Tally to_bits("signed_bits");
    Tally from_bits("signed_from_bits");
    Sequence s(25u);
    for (unsigned i = 0u; i < kSamples + 2u * kEdgeCount; i++) {
        const unsigned e = i - kSamples;
        const double v =
            i < kSamples ? random_signed(s, i) : kEdges[e / 2u] * (e % 2u ? -1.0 : 1.0);
        to_bits.compare(vmaf_mtl_signed_bits(signed_of(v)), expected_bits(v), v, 0.0);
        from_bits.compare(bits_of_signed(vmaf_mtl_signed_from_bits(bits_of(v))), expected_bits(v),
                          v, 0.0);
    }
    mu_assert("vmaf_mtl_signed_bits() is not the value's bit pattern", to_bits.passed(kSamples));
    mu_assert("vmaf_mtl_signed_from_bits() is not the pattern's value", from_bits.passed(kSamples));
    return nullptr;
}

/* An integer of a random width, or one next to 2^53 / 2^64 where fp64
 * rounds: 2^53 + 1 and 2^53 + 3 are ties. */
uint64_t random_u64(Sequence &s, unsigned i)
{
    const uint64_t edges[] = {0u,
                              1u,
                              (UINT64_C(1) << 53) - 1u,
                              UINT64_C(1) << 53,
                              (UINT64_C(1) << 53) + 1u,
                              (UINT64_C(1) << 53) + 3u,
                              (UINT64_C(1) << 54) + 2u,
                              (UINT64_C(1) << 54) + 6u,
                              UINT64_C(1) << 63,
                              ~UINT64_C(0)};
    if (i < 10u)
        return edges[i];
    const uint64_t v = s.next() >> s.below(64);
    return (i % 5u) == 0u ? (v | 1u) << s.below(12) : v;
}

mu_message_t test_signed_from_integers_are_the_conversions()
{
    Tally wide("signed_from_u64");
    Tally exact("signed_from_exact");
    Sequence s(26u);
    for (unsigned i = 0u; i < kSamples; i++) {
        const uint64_t v = random_u64(s, i);
        wide.compare(bits_of_signed(vmaf_mtl_signed_from_u64(v)), bits_of((double)v), (double)v,
                     0.0);
        const uint64_t small = v >> 11;
        exact.compare(bits_of_signed(vmaf_mtl_signed_from_exact(small)), bits_of((double)small),
                      (double)small, 0.0);
    }
    mu_assert("vmaf_mtl_signed_from_u64() is not fl64(integer)", wide.passed(kSamples));
    mu_assert("vmaf_mtl_signed_from_exact() is not the integer's value", exact.passed(kSamples));
    return nullptr;
}

/* Every finite fp32 class: random patterns, both zeros, the subnormal and
 * normal ranges' ends. */
mu_message_t test_signed_from_float_is_exact()
{
    Tally tally("signed_from_float");
    Sequence s(27u);
    const uint32_t edges[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu,
                              0x00800000u, 0x3F800000u, 0xFF7FFFFFu};
    for (unsigned i = 0u; i < kSamples + 7u; i++) {
        uint32_t bits = i < 7u ? edges[i] : (uint32_t)s.next();
        if (((bits >> 23) & 0xFFu) == 0xFFu)
            bits ^= 0x40000000u; /* not an infinity or a NaN */
        const float x = fvalue_of(bits);
        tally.compare(bits_of_signed(vmaf_mtl_signed_from_float(x)), expected_bits((double)x),
                      (double)x, 0.0);
    }
    mu_assert("vmaf_mtl_signed_from_float() is not the float's value", tally.passed(kSamples));
    return nullptr;
}

mu_message_t test_signed_unary_operations()
{
    Tally negate("signed_negate");
    Tally absolute("signed_abs");
    Tally twice("signed_twice");
    Sequence s(28u);
    for (unsigned i = 0u; i < kSamples; i++) {
        const double v = random_signed(s, i);
        const VmafMtlSoftSigned x = signed_of(v);
        negate.compare(bits_of_signed(vmaf_mtl_signed_negate(x)), expected_bits(-v), v, 0.0);
        absolute.compare(bits_of_signed(vmaf_mtl_signed_abs(x)), expected_bits(std::fabs(v)), v,
                         0.0);
        /* |v| < 2^1023: 2 * v is finite. */
        twice.compare(bits_of_signed(vmaf_mtl_signed_twice(x)), expected_bits(2.0 * v), v, 0.0);
    }
    mu_assert("vmaf_mtl_signed_negate() is not -a", negate.passed(kSamples));
    mu_assert("vmaf_mtl_signed_abs() is not |a|", absolute.passed(kSamples));
    mu_assert("vmaf_mtl_signed_twice() is not 2 * a", twice.passed(kSamples));
    return nullptr;
}

/* ------------------------------------------------------------------ */
/* 128-bit helpers, against bit-by-bit references                      */
/* ------------------------------------------------------------------ */

bool bit_of(VmafMtlU128 a, unsigned i)
{
    if (i >= 128u)
        return false;
    return ((i >= 64u ? a.hi >> (i - 64u) : a.lo >> i) & 1u) != 0u;
}

void set_bit(VmafMtlU128 &a, unsigned i)
{
    if (i < 64u)
        a.lo |= UINT64_C(1) << i;
    else if (i < 128u)
        a.hi |= UINT64_C(1) << (i - 64u);
}

/* a * b from 16-bit digits. */
VmafMtlU128 reference_mul(uint64_t a, uint64_t b)
{
    uint64_t digit[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    for (unsigned i = 0u; i < 4u; i++) {
        for (unsigned j = 0u; j < 4u; j++) {
            digit[i + j] += ((a >> (16u * i)) & 0xFFFFu) * ((b >> (16u * j)) & 0xFFFFu);
        }
    }
    VmafMtlU128 r = {0u, 0u};
    uint64_t carry = 0u;
    for (unsigned k = 0u; k < 8u; k++) {
        const uint64_t total = digit[k] + carry;
        const uint64_t part = total & 0xFFFFu;
        carry = total >> 16;
        if (k < 4u)
            r.lo |= part << (16u * k);
        else
            r.hi |= part << (16u * (k - 4u));
    }
    return r;
}

VmafMtlU128 reference_shl(uint64_t value, unsigned shift)
{
    VmafMtlU128 r = {0u, 0u};
    for (unsigned i = 0u; i < 64u; i++) {
        if (((value >> i) & 1u) != 0u)
            set_bit(r, i + shift);
    }
    return r;
}

VmafMtlShifted reference_shr_round(VmafMtlU128 a, unsigned shift)
{
    VmafMtlShifted r = {0u, false, false};
    for (unsigned j = 0u; j < 64u; j++) {
        r.kept |= bit_of(a, shift + j) ? UINT64_C(1) << j : UINT64_C(0);
    }
    r.halfway = shift > 0u && bit_of(a, shift - 1u);
    for (unsigned i = 0u; i + 1u < shift; i++) {
        r.sticky = r.sticky || bit_of(a, i);
    }
    return r;
}

/* a - b by a ripple borrow over 128 bits. */
VmafMtlU128 reference_sub(VmafMtlU128 a, uint64_t b)
{
    VmafMtlU128 r = {0u, 0u};
    unsigned borrow = 0u;
    for (unsigned i = 0u; i < 128u; i++) {
        const unsigned x = bit_of(a, i) ? 1u : 0u;
        const unsigned y = i < 64u ? (unsigned)((b >> i) & 1u) : 0u;
        const unsigned d = x - y - borrow;
        borrow = (x < y + borrow) ? 1u : 0u;
        if ((d & 1u) != 0u)
            set_bit(r, i);
    }
    return r;
}

bool same_u128(VmafMtlU128 a, VmafMtlU128 b)
{
    return a.hi == b.hi && a.lo == b.lo;
}

/* A random 64-bit operand: a random width, all ones, or a lone bit. */
uint64_t random_word(Sequence &s, unsigned i)
{
    const unsigned kind = i % 4u;
    if (kind == 1u)
        return ~UINT64_C(0) >> s.below(64);
    if (kind == 2u)
        return UINT64_C(1) << s.below(64);
    return s.next() >> s.below(64);
}

mu_message_t test_u128_mul_is_the_full_product()
{
    Sequence s(31u);
    unsigned wrong = 0u;
    for (unsigned i = 0u; i < kSamples; i++) {
        const uint64_t a = random_word(s, i);
        const uint64_t b = random_word(s, i / 4u);
        wrong += same_u128(vmaf_mtl_u128_mul(a, b), reference_mul(a, b)) ? 0u : 1u;
    }
    mu_assert("vmaf_mtl_u128_mul() is not the 128-bit product", wrong == 0u);
    return nullptr;
}

/* Every shift 0 to 127 of shl and shr_round on random values, msb and sub
 * on random values. */
mu_message_t test_u128_shifts_msb_and_sub()
{
    Sequence s(32u);
    unsigned wrong[4] = {0u, 0u, 0u, 0u};
    for (unsigned i = 0u; i < 128u * 8192u; i++) {
        const unsigned shift = i % 128u;
        const VmafMtlU128 a = {random_word(s, i), random_word(s, i + 1u)};
        const uint64_t v = random_word(s, i + 2u);
        wrong[0] += same_u128(vmaf_mtl_u128_shl(v, shift), reference_shl(v, shift)) ? 0u : 1u;
        const VmafMtlShifted got = vmaf_mtl_u128_shr_round(a, shift);
        const VmafMtlShifted want = reference_shr_round(a, shift);
        wrong[1] +=
            got.kept == want.kept && got.halfway == want.halfway && got.sticky == want.sticky ? 0u :
                                                                                                1u;
        unsigned msb = 127u;
        for (; msb > 0u && !bit_of(a, msb); msb--) {
        }
        wrong[2] += (a.hi | a.lo) == 0u || vmaf_mtl_u128_msb(a) == msb ? 0u : 1u;
        wrong[3] += same_u128(vmaf_mtl_u128_sub(a, v), reference_sub(a, v)) ? 0u : 1u;
    }
    mu_assert("vmaf_mtl_u128_shl() is not the shifted value", wrong[0] == 0u);
    mu_assert("vmaf_mtl_u128_shr_round() is not the kept, half and sticky bits", wrong[1] == 0u);
    mu_assert("vmaf_mtl_u128_msb() is not the highest set bit", wrong[2] == 0u);
    mu_assert("vmaf_mtl_u128_sub() is not the 128-bit difference", wrong[3] == 0u);
    return nullptr;
}

/* round_kept() and soft_round() at each of their boundaries. */
mu_message_t test_rounding_helpers()
{
    const VmafMtlShifted cases[] = {
        vmaf_mtl_shifted_make(4u, true, false), /* tie, even: stays */
        vmaf_mtl_shifted_make(5u, true, false), /* tie, odd: up */
        vmaf_mtl_shifted_make(4u, true, true),  /* above half: up */
        vmaf_mtl_shifted_make(5u, false, true), /* below half: stays */
    };
    const uint64_t want[] = {4u, 6u, 5u, 5u};
    for (unsigned i = 0u; i < 4u; i++) {
        mu_assert("vmaf_mtl_round_kept() does not round to nearest even",
                  vmaf_mtl_round_kept(cases[i]) == want[i]);
    }
    /* (2^53 - 1) with guard bits 100: a tie of an odd value, carries out. */
    const VmafMtlSoftDouble carry = vmaf_mtl_soft_round((((kTop << 1) - 1u) << 3) | 4u, -3);
    mu_assert("vmaf_mtl_soft_round() does not renormalise a carry",
              carry.mant == kTop && carry.exp == 1);
    const VmafMtlSoftDouble even = vmaf_mtl_soft_round((kTop << 3) | 4u, 0);
    mu_assert("vmaf_mtl_soft_round() does not keep an even tie",
              even.mant == kTop && even.exp == 3);
    return nullptr;
}

} // namespace

mu_message_t run_tests(void)
{
    mu_run_test(test_u128_mul_is_the_full_product);
    mu_run_test(test_u128_shifts_msb_and_sub);
    mu_run_test(test_rounding_helpers);
    mu_run_test(test_soft_add_is_the_fp64_sum);
    mu_run_test(test_soft_mul_is_the_fp64_product);
    mu_run_test(test_soft_div_is_the_fp64_quotient);
    mu_run_test(test_soft_div_digits_is_soft_div);
    mu_run_test(test_soft_less_is_the_fp64_comparison);
    mu_run_test(test_soft_trunc_is_the_conversion);
    mu_run_test(test_soft_sub_trunc_is_the_truncated_difference);
    mu_run_test(test_soft_from_float_is_exact);
    mu_run_test(test_soft_from_float_any_takes_subnormals);
    mu_run_test(test_soft_from_u32_is_exact);
    mu_run_test(test_soft_to_float_rounds_to_nearest_even);
    mu_run_test(test_soft_to_float_any_covers_every_regime);
    mu_run_test(test_signed_add_sub_are_the_fp64_operations);
    mu_run_test(test_signed_mul_div_are_the_fp64_operations);
    mu_run_test(test_signed_bits_round_trip);
    mu_run_test(test_signed_from_integers_are_the_conversions);
    mu_run_test(test_signed_from_float_is_exact);
    mu_run_test(test_signed_unary_operations);
    return nullptr;
}
