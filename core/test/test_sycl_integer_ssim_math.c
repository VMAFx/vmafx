/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-1443: the per-pixel term integer_ssim_sycl's kernel computes, against
 * the reference's fp64 expression, on the host and on the device.
 *
 * integer_ssim.c::ssim_reduce_row_range() forms the term in fp64 from a
 * window's int64 moments:
 *
 *     w_d = m.w;
 *     c1 = sm * sm * SSIM_K1 * w_d * w_d;
 *     c2 = sm * sm * SSIM_K2 * w_d * w_d;
 *     mx2 = m.mux * (double)m.mux;
 *     mxy = m.mux * (double)m.muy;
 *     my2 = m.muy * (double)m.muy;
 *     *ssim += m.w * (2 * mxy + c1) * (c2 + 2 * (m.xy * w_d - mxy)) /
 *              ((mx2 + my2 + c1) * (m.x2 * w_d - mx2 + m.y2 * w_d - my2 + c2));
 *
 * feature/sycl/sycl_integer_ssim_math.h returns that double's bit pattern
 * without an fp64 type (ADR-0220): every fp64 operation of the expression is
 * done on a significand and an exponent in 64-bit integers
 * (feature/sycl/sycl_soft_signed.h). This test checks, through
 * test_sycl_integer_ssim_math_probe.cpp, with reference_term() below holding
 * the reference's lines verbatim:
 *
 *   - each operation of sycl_soft_signed.h against the host's fp64 operation:
 *     random operands, operands that cancel, operands a few units in the
 *     last place apart, integers up to 2^64, ties, zeros;
 *   - the term at 8, 10, 12 and 16 bits over windows of every truncation,
 *     flat, textured, identical and inverted: at 8 and 10 bits every product
 *     of two moments is below 2^52 and the header's integer path is taken,
 *     at 16 bits the rounding path, at 12 bits both;
 *   - the same in a kernel on the default GPU, where the quotient's digits
 *     come from the device's fp32 division.
 *
 * test_sycl_ssim_exact_contract.py pins that integer_ssim.c still holds the
 * lines reference_term() copies.
 *
 * A zero has no sign in sycl_soft_signed.h (the reference adds its terms into
 * a sum, where the sign of a zero changes nothing), so a zero result is
 * compared by value.
 *
 * Skip behaviour: the host checks always run; without a SYCL GPU the device
 * checks are skipped and the test exits 77.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

void vmaf_test_sycl_soft_host(const uint64_t *a, const uint64_t *b, size_t n, int op,
                              uint64_t *out);
int vmaf_test_sycl_soft_device(const uint64_t *a, const uint64_t *b, size_t n, int op,
                               uint64_t *out);
void vmaf_test_sycl_issim_host(const uint64_t *moments, size_t n, unsigned bpc, uint64_t *out);
int vmaf_test_sycl_issim_device(const uint64_t *moments, size_t n, unsigned bpc, uint64_t *out);

enum {
    OP_SAMPLES = 400000,
    TERM_SAMPLES = 300000,
    MOMENT_COUNT = 6,
    DEPTH_COUNT = 4,
    KERNEL_TAPS = 9,
    OP_ADD = 0,
    OP_SUB = 1,
    OP_MUL = 2,
    OP_DIV = 3,
    OP_FROM_U64 = 4,
    OP_COUNT = 5,
    OPERAND_KINDS = 8,
    WINDOW_KINDS = 6,
};

/* The bound below which the header takes its integer path. */
#define EXACT_PRODUCT_BOUND (1ULL << 52)

static const unsigned DEPTH[DEPTH_COUNT] = {8u, 10u, 12u, 16u};
static const char *const OP_NAME[OP_COUNT] = {"add", "sub", "mul", "div", "from_u64"};
/* gaussian_filter_init(1.5, 5): integer_ssim.c's kernel. */
static const int64_t KERNEL[KERNEL_TAPS] = {2, 9, 28, 55, 68, 55, 28, 9, 2};

static uint64_t op_a[OP_SAMPLES];
static uint64_t op_b[OP_SAMPLES];
static uint64_t op_ref[OP_SAMPLES];
static uint64_t op_got[OP_SAMPLES];
static uint64_t moments[(size_t)TERM_SAMPLES * MOMENT_COUNT];
static uint64_t term_ref[TERM_SAMPLES];
static uint64_t term_got[TERM_SAMPLES];

/* integer_ssim.c's ssim_moments, SSIM_K1 and SSIM_K2. */
typedef struct ssim_moments {
    int64_t mux;
    int64_t muy;
    int64_t x2;
    int64_t xy;
    int64_t y2;
    int64_t w;
} ssim_moments;

#define SSIM_K1 (0.01 * 0.01)
#define SSIM_K2 (0.03 * 0.03)

/* integer_ssim.c::ssim_reduce_row_range(), the lines that form one pixel's
 * term, verbatim; the reference adds the returned expression to `*ssim`. */
static double reference_term(ssim_moments m, int samplemax)
{
    const double sm = (double)samplemax;
    double c1;
    double c2;
    double mx2;
    double mxy;
    double my2;
    double w_d;
    w_d = (double)m.w;
    c1 = sm * sm * SSIM_K1 * w_d * w_d;
    c2 = sm * sm * SSIM_K2 * w_d * w_d;
    mx2 = m.mux * (double)m.mux;
    mxy = m.mux * (double)m.muy;
    my2 = m.muy * (double)m.muy;
    return m.w * (2 * mxy + c1) * (c2 + 2 * (m.xy * w_d - mxy)) /
           ((mx2 + my2 + c1) * (m.x2 * w_d - mx2 + m.y2 * w_d - my2 + c2));
}

static uint64_t bits_of(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static double value_of(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/* Deterministic generator: the same inputs on every host. */
static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint64_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* A normal double: a random significand at a binary exponent in
 * [-40, 80), of a random sign. */
static double random_double(void)
{
    const double significand = (double)((rng_next() >> 11) | (1ULL << 52));
    const int exponent = (int)(rng_next() % 120u) - 40;
    const double value = ldexp(significand, exponent - 52);
    return (rng_next() & 1u) ? -value : value;
}

/* A double a few units in the last place from `a`, of the opposite sign: the
 * sum cancels to a few bits. */
static double beside(double a)
{
    const uint64_t steps = 1u + (rng_next() % 3u);
    const uint64_t bits = bits_of(a) + ((rng_next() & 1u) ? steps : (uint64_t)0 - steps);
    return -value_of(bits);
}

/* An integer of a random width up to 64 bits, as a double. */
static double random_integer(void)
{
    return (double)(rng_next() >> (rng_next() % 64u));
}

/* One pair of operands; `kind` cycles through the cases of the header. */
static void fill_operands(unsigned kind, double *a, double *b)
{
    *a = random_double();
    *b = random_double();
    switch (kind % OPERAND_KINDS) {
    case 0: /* the exponents at most one apart: an exact difference */
        *b = ldexp(*b, ilogb(*a) - ilogb(*b) + (int)(rng_next() % 3u) - 1);
        break;
    case 1: /* a few units in the last place apart */
        *b = beside(*a);
        break;
    case 2: /* integers */
        *a = random_integer();
        *b = random_integer();
        break;
    case 3: /* the smaller operand far below the larger's last place */
        *b = ldexp(*b, ilogb(*a) - ilogb(*b) - 50 - (int)(rng_next() % 20u));
        break;
    case 4: /* a tie: half a unit in the last place of an odd or even value */
        *b = ldexp(1.0, ilogb(*a) - 53);
        break;
    case 5: /* a zero */
        *a = 0.0;
        break;
    default: /* random */
        break;
    }
}

/* fl64(a op b) on the host: the operation the header stands for. */
static double reference_operation(int op, double a, double b, uint64_t integer)
{
    switch (op) {
    case OP_ADD:
        return a + b;
    case OP_SUB:
        return a - b;
    case OP_MUL:
        return a * b;
    case OP_DIV:
        return a / b;
    default:
        return (double)integer;
    }
}

static void fill_operations(int op)
{
    rng_state = 0x9e3779b97f4a7c15ULL;
    for (unsigned i = 0u; i < OP_SAMPLES; i++) {
        double a = 0.0;
        double b = 0.0;
        fill_operands(i, &a, &b);
        if (op == OP_DIV && b == 0.0)
            b = 3.0;
        const uint64_t integer = rng_next() >> (rng_next() % 64u);
        op_a[i] = op == OP_FROM_U64 ? integer : bits_of(a);
        op_b[i] = bits_of(b);
        op_ref[i] = bits_of(reference_operation(op, a, b, integer));
    }
}

/* Equal bits, or two zeros. */
static int same_double(uint64_t got, uint64_t ref)
{
    return got == ref || (value_of(got) == 0.0 && value_of(ref) == 0.0);
}

static unsigned count_wrong_operations(const char *where, int op)
{
    unsigned wrong = 0u;
    for (unsigned i = 0u; i < OP_SAMPLES; i++) {
        if (same_double(op_got[i], op_ref[i]))
            continue;
        if (wrong < 5u) {
            (void)fprintf(stderr, "\n%s %s sample %u: %a, %a (%llu): %a, reference %a", where,
                          OP_NAME[op], i, value_of(op_a[i]), value_of(op_b[i]),
                          (unsigned long long)op_a[i], value_of(op_got[i]), value_of(op_ref[i]));
        }
        wrong++;
    }
    if (wrong != 0u) {
        (void)fprintf(stderr, "\n%s %s: %u of %d samples differ\n", where, OP_NAME[op], wrong,
                      OP_SAMPLES);
    }
    return wrong;
}

/* The sample of one pixel of a window. `kind`: 0 and 1 a flat window, 2 a
 * textured one, 3 the full range; the comparison is the reference itself
 * (4), its negative (5) or its own sample. */
static void window_samples(unsigned kind, int samplemax, int base, int64_t *s, int64_t *d)
{
    const int span = kind < 2u ? 4 : (kind == 2u ? samplemax / 8 + 1 : samplemax + 1);
    const int low = kind == 3u ? 0 : base;
    int64_t reference = low + (int64_t)(rng_next() % (uint64_t)span);
    int64_t comparison = low + (int64_t)(rng_next() % (uint64_t)span);
    reference = reference > samplemax ? samplemax : reference;
    comparison = comparison > samplemax ? samplemax : comparison;
    if (kind == 4u)
        comparison = reference;
    if (kind == 5u)
        comparison = samplemax - reference;
    *s = reference;
    *d = comparison;
}

/* The moments of one random window, as ssim_accumulate_row() and
 * ssim_reduce_row_range() accumulate them. One window in four is truncated
 * as at a frame's edge. */
static ssim_moments random_window(unsigned index, int samplemax)
{
    const unsigned kind = index % WINDOW_KINDS;
    const int truncated = (index % 4u) == 0u;
    const int first_row = truncated ? (int)(rng_next() % 5u) : 0;
    const int last_row = KERNEL_TAPS - (truncated ? (int)(rng_next() % 5u) : 0);
    const int first_col = truncated ? (int)(rng_next() % 5u) : 0;
    const int last_col = KERNEL_TAPS - (truncated ? (int)(rng_next() % 5u) : 0);
    const int base = (int)(rng_next() % (uint64_t)(samplemax + 1));
    ssim_moments m = {0};
    for (int row = first_row; row < last_row; row++) {
        for (int col = first_col; col < last_col; col++) {
            int64_t s = 0;
            int64_t d = 0;
            window_samples(kind, samplemax, base, &s, &d);
            const int64_t window = KERNEL[row] * KERNEL[col];
            m.mux += window * s;
            m.muy += window * d;
            m.x2 += window * s * s;
            m.xy += window * s * d;
            m.y2 += window * d * d;
            m.w += window;
        }
    }
    return m;
}

/* What the samples of one bit depth cover. */
typedef struct Coverage {
    unsigned exact;    /* every product of two moments below 2^52 */
    unsigned negative; /* the term is below zero */
} Coverage;

static int products_exact(ssim_moments m)
{
    const uint64_t all = (uint64_t)(m.mux * m.mux) | (uint64_t)(m.mux * m.muy) |
                         (uint64_t)(m.muy * m.muy) | (uint64_t)(m.x2 * m.w) |
                         (uint64_t)(m.xy * m.w) | (uint64_t)(m.y2 * m.w);
    return all < EXACT_PRODUCT_BOUND;
}

static Coverage fill_terms(unsigned bpc)
{
    const int samplemax = (1 << bpc) - 1;
    Coverage coverage = {0u, 0u};
    rng_state = 0x9e3779b97f4a7c15ULL;
    for (unsigned i = 0u; i < TERM_SAMPLES; i++) {
        const ssim_moments m = random_window(i, samplemax);
        uint64_t *out = &moments[(size_t)i * MOMENT_COUNT];
        out[0] = (uint64_t)m.mux;
        out[1] = (uint64_t)m.muy;
        out[2] = (uint64_t)m.x2;
        out[3] = (uint64_t)m.xy;
        out[4] = (uint64_t)m.y2;
        out[5] = (uint64_t)m.w;
        const double term = reference_term(m, samplemax);
        term_ref[i] = bits_of(term);
        /* 16-bit products exceed int64 only as unsigned values. */
        coverage.exact += (bpc < 16u && products_exact(m)) ? 1u : 0u;
        coverage.negative += term < 0.0 ? 1u : 0u;
    }
    return coverage;
}

static unsigned count_wrong_terms(const char *where, unsigned bpc)
{
    unsigned wrong = 0u;
    for (unsigned i = 0u; i < TERM_SAMPLES; i++) {
        if (same_double(term_got[i], term_ref[i]))
            continue;
        if (wrong < 5u) {
            const uint64_t *m = &moments[(size_t)i * MOMENT_COUNT];
            (void)fprintf(stderr,
                          "\n%s %u-bit sample %u mux=%llu muy=%llu x2=%llu xy=%llu y2=%llu "
                          "w=%llu: %a, reference %a",
                          where, bpc, i, (unsigned long long)m[0], (unsigned long long)m[1],
                          (unsigned long long)m[2], (unsigned long long)m[3],
                          (unsigned long long)m[4], (unsigned long long)m[5], value_of(term_got[i]),
                          value_of(term_ref[i]));
        }
        wrong++;
    }
    if (wrong != 0u) {
        (void)fprintf(stderr, "\n%s %u-bit: %u of %d terms differ\n", where, bpc, wrong,
                      TERM_SAMPLES);
    }
    return wrong;
}

static char *test_operations_are_the_fp64_operations(void)
{
    for (int op = 0; op < OP_COUNT; op++) {
        fill_operations(op);
        vmaf_test_sycl_soft_host(op_a, op_b, OP_SAMPLES, op, op_got);
        mu_assert("an operation of sycl_soft_signed.h is not the fp64 operation",
                  count_wrong_operations("host", op) == 0u);
    }
    return NULL;
}

static char *test_terms_are_the_fp64_expression(void)
{
    for (unsigned depth = 0u; depth < DEPTH_COUNT; depth++) {
        const unsigned bpc = DEPTH[depth];
        const Coverage coverage = fill_terms(bpc);
        vmaf_test_sycl_issim_host(moments, TERM_SAMPLES, bpc, term_got);
        mu_assert("the term is not the reference's fp64 expression",
                  count_wrong_terms("host", bpc) == 0u);
        mu_assert("no term is negative: the inverted windows are not reached",
                  coverage.negative > 0u);
        if (bpc <= 10u) {
            mu_assert("an 8- or 10-bit window leaves the integer path",
                      coverage.exact == (unsigned)TERM_SAMPLES);
        }
        if (bpc == 12u) {
            mu_assert("the 12-bit windows do not reach both paths",
                      coverage.exact > 0u && coverage.exact < (unsigned)TERM_SAMPLES);
        }
    }
    return NULL;
}

static char *test_device_operations_are_the_fp64_operations(void)
{
    for (int op = 0; op < OP_COUNT; op++) {
        fill_operations(op);
        const int err = vmaf_test_sycl_soft_device(op_a, op_b, OP_SAMPLES, op, op_got);
        if (err == -ENODEV) {
            (void)fprintf(stderr, "[skip: no SYCL GPU] ");
            mu_skipped = 1;
            return NULL;
        }
        mu_assert("the device operation kernel failed", err == 0);
        mu_assert("an operation differs from the fp64 operation on the device",
                  count_wrong_operations("device", op) == 0u);
    }
    return NULL;
}

static char *test_device_terms_are_the_fp64_expression(void)
{
    for (unsigned depth = 0u; depth < DEPTH_COUNT; depth++) {
        const unsigned bpc = DEPTH[depth];
        (void)fill_terms(bpc);
        const int err = vmaf_test_sycl_issim_device(moments, TERM_SAMPLES, bpc, term_got);
        if (err == -ENODEV) {
            (void)fprintf(stderr, "[skip: no SYCL GPU] ");
            mu_skipped = 1;
            return NULL;
        }
        mu_assert("the device term kernel failed", err == 0);
        mu_assert("the term differs from the reference's fp64 expression on the device",
                  count_wrong_terms("device", bpc) == 0u);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_operations_are_the_fp64_operations);
    mu_run_test(test_terms_are_the_fp64_expression);
    mu_run_test(test_device_operations_are_the_fp64_operations);
    mu_run_test(test_device_terms_are_the_fp64_expression);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
