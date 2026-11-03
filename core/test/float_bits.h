/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Bit identity of floating-point values, for the tests that assert a result
 * has another computation's bits: a GPU or SIMD twin against the CPU
 * extractor, a replay against the reference, a score against a recorded one.
 *
 * `==` is not that test. It calls +0 and -0 equal, which two computations
 * that differ in one sign can return, and it calls a NaN unequal to itself.
 * vmaf_test_identical_f64() compares the bit patterns (memcpy into an
 * unsigned integer of the same width) and treats a NaN as identical to
 * nothing, as `==` does: it holds exactly when `a == b` holds and the two
 * have the same bits, so it is never weaker than the `==` it replaces, and a
 * test that failed on a NaN still fails on one. The negation is "the two are
 * told apart": different bits, or a NaN.
 *
 * vmaf_test_expect_identical_f64() prints one line on a mismatch with both
 * values at %.17g and their bits, for an assertion that has no report of its
 * own. The _f32 forms do the same for float.
 * vmaf_test_within_f64() is the tolerance form: bit identity at zero
 * tolerance, |a - b| <= tol above it.
 *
 * C and C++, header-only; core/test/test_float_bits.c tests it.
 */

#ifndef LIBVMAF_TEST_FLOAT_BITS_H_
#define LIBVMAF_TEST_FLOAT_BITS_H_

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static inline uint64_t vmaf_test_bits_f64(double value)
{
    uint64_t bits = 0u;
    (void)memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static inline uint32_t vmaf_test_bits_f32(float value)
{
    uint32_t bits = 0u;
    (void)memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* `a` and `b` are the same number to the last bit; a NaN never is. */
static inline bool vmaf_test_identical_f64(double a, double b)
{
    return !isnan(a) && vmaf_test_bits_f64(a) == vmaf_test_bits_f64(b);
}

static inline bool vmaf_test_identical_f32(float a, float b)
{
    return !isnan(a) && vmaf_test_bits_f32(a) == vmaf_test_bits_f32(b);
}

/* `a` and `b` agree within `tol`: bit identity when `tol` is zero (so +0 and
 * -0 still differ), else identity or |a - b| <= tol. A NaN agrees with
 * nothing. */
static inline bool vmaf_test_within_f64(double a, double b, double tol)
{
    return vmaf_test_identical_f64(a, b) || (tol > 0.0 && fabs(a - b) <= tol);
}

/* vmaf_test_identical_f64(), and on a mismatch one line on stderr:
 * "<what>: <a> (0x<bits>) is not <b> (0x<bits>)". */
static inline bool vmaf_test_expect_identical_f64(const char *what, double a, double b)
{
    if (vmaf_test_identical_f64(a, b))
        return true;
    (void)fprintf(stderr, "\n%s: %.17g (0x%016" PRIx64 ") is not %.17g (0x%016" PRIx64 ")\n", what,
                  a, vmaf_test_bits_f64(a), b, vmaf_test_bits_f64(b));
    return false;
}

static inline bool vmaf_test_expect_identical_f32(const char *what, float a, float b)
{
    if (vmaf_test_identical_f32(a, b))
        return true;
    (void)fprintf(stderr, "\n%s: %.17g (0x%08" PRIx32 ") is not %.17g (0x%08" PRIx32 ")\n", what,
                  (double)a, vmaf_test_bits_f32(a), (double)b, vmaf_test_bits_f32(b));
    return false;
}

#endif /* LIBVMAF_TEST_FLOAT_BITS_H_ */
