/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  ADR-1367: the SYCL feature line gives device kernels the CPU reference's
 *  fp32 arithmetic. A multiply-add written as one expression is not contracted
 *  into an FMA, and `/` and sqrt are correctly rounded. The kernel lives in
 *  test_sycl_fp_arith_probe.cpp, compiled like an extractor TU; this side builds
 *  the operands and the correctly rounded host references. fp32 operations
 *  evaluated in fp64 and rounded once are correctly rounded (53 >= 2 * 24 + 2).
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

int vmaf_test_sycl_fp_arith(const float *a, const float *b, const float *c, size_t n, float *mad,
                            float *quot, float *root);

enum {
    RANDOM_COUNT = 1 << 20,
    BOUNDARY_VALUES = 13,
    BOUNDARY_COUNT = BOUNDARY_VALUES * BOUNDARY_VALUES * BOUNDARY_VALUES,
};

typedef struct {
    float *a;
    float *b;
    float *c;
    float *mad;
    float *quot;
    float *root;
    size_t n;
} Operands;

typedef struct {
    size_t mad;
    size_t quot;
    size_t root;
} Mismatches;

static uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Random sign and mantissa, exponent in [-20, 20]: products and quotients stay
 * normal, so every mismatch is a rounding difference. */
static float random_operand(uint32_t *state)
{
    const uint32_t mantissa = next_random(state) & 0x7fffffu;
    const uint32_t exponent = (next_random(state) % 41u) + 107u;
    const uint32_t sign = (next_random(state) & 1u) << 31;
    const uint32_t bits = sign | (exponent << 23) | mantissa;
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/* Bit-for-bit equality; every NaN counts as the same result. */
static int same_float(float x, float y)
{
    uint32_t xb;
    uint32_t yb;
    memcpy(&xb, &x, sizeof(xb));
    memcpy(&yb, &y, sizeof(yb));
    return xb == yb || (isnan(x) && isnan(y));
}

static Mismatches count_mismatches(const Operands *op)
{
    Mismatches m = {0, 0, 0};
    for (size_t i = 0; i < op->n; i++) {
        const float product = (float)((double)op->a[i] * (double)op->b[i]);
        const float mad = (float)((double)product + (double)op->c[i]);
        const float quot = (float)((double)op->a[i] / (double)op->b[i]);
        const float root = (float)sqrt((double)fabsf(op->a[i]));
        m.mad += !same_float(mad, op->mad[i]);
        m.quot += !same_float(quot, op->quot[i]);
        m.root += !same_float(root, op->root[i]);
    }
    return m;
}

static int operands_alloc(Operands *op, size_t n)
{
    float *block = calloc(6 * n, sizeof(float));
    if (!block)
        return -ENOMEM;
    *op = (Operands){block,           block + n, block + (2 * n), block + (3 * n), block + (4 * n),
                     block + (5 * n), n};
    return 0;
}

/* Runs the device probe; NULL when the check passed or was skipped. */
static char *run_and_check(Operands *op, const char *label)
{
    const int err =
        vmaf_test_sycl_fp_arith(op->a, op->b, op->c, op->n, op->mad, op->quot, op->root);
    if (err == -ENODEV) {
        (void)fprintf(stderr, "  [SKIP] %s: no SYCL GPU device\n", label);
        mu_skipped = 1;
        return NULL;
    }
    mu_assert("device probe failed", err == 0);
    const Mismatches m = count_mismatches(op);
    (void)fprintf(stderr, "  %s: %zu operands, mismatches mad=%zu div=%zu sqrt=%zu\n", label, op->n,
                  m.mad, m.quot, m.root);
    mu_assert("a * b + c was contracted into an FMA on the device", m.mad == 0);
    mu_assert("device fp32 division is not correctly rounded", m.quot == 0);
    mu_assert("device fp32 sqrt is not correctly rounded", m.root == 0);
    return NULL;
}

static char *test_invalid_arguments(void)
{
    float x = 1.0f;
    mu_assert("NULL operand must be rejected",
              vmaf_test_sycl_fp_arith(NULL, &x, &x, 1, &x, &x, &x) == -EINVAL);
    mu_assert("NULL result must be rejected",
              vmaf_test_sycl_fp_arith(&x, &x, &x, 1, &x, &x, NULL) == -EINVAL);
    mu_assert("an empty probe is a no-op", vmaf_test_sycl_fp_arith(&x, &x, &x, 0, &x, &x, &x) == 0);
    mu_assert("an empty probe leaves results alone", x == 1.0f);
    return NULL;
}

static char *test_random_operands(void)
{
    Operands op;
    mu_assert("operand allocation failed", operands_alloc(&op, RANDOM_COUNT) == 0);
    uint32_t state = 12345u;
    for (size_t i = 0; i < op.n; i++) {
        op.a[i] = random_operand(&state);
        op.b[i] = random_operand(&state);
        op.c[i] = random_operand(&state);
    }
    char *msg = run_and_check(&op, "random");
    free(op.a);
    return msg;
}

/* Every combination of unit, near-unit, extreme and range-edge values: exact
 * results, overflow to infinity, the smallest normal and the top of the range. */
static char *test_boundary_operands(void)
{
    static const float values[BOUNDARY_VALUES] = {
        1.0f,     3.0f,     7.0f,      0.1f,      -2.5f,     0x1.fffffep-1f,   0x1.000002p+0f,
        0x1p-63f, 0x1p+63f, 0x1p-126f, 0x1p-100f, 0x1p+100f, 0x1.fffffep+127f,
    };
    Operands op;
    mu_assert("operand allocation failed", operands_alloc(&op, BOUNDARY_COUNT) == 0);
    size_t k = 0;
    for (size_t i = 0; i < BOUNDARY_VALUES; i++) {
        for (size_t j = 0; j < BOUNDARY_VALUES; j++) {
            for (size_t l = 0; l < BOUNDARY_VALUES; l++, k++) {
                op.a[k] = values[i];
                op.b[k] = values[j];
                op.c[k] = values[l];
            }
        }
    }
    char *msg = run_and_check(&op, "boundary");
    free(op.a);
    return msg;
}

char *run_tests(void)
{
    mu_run_test(test_invalid_arguments);
    mu_run_test(test_random_operands);
    mu_run_test(test_boundary_operands);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
