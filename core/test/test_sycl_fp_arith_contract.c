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
 *
 *  ADR-1407: the HIP kernels make the same promise through hip_strict_fp_args.
 *  test_hip_fp_arith_contract is this file built with
 *  -DFP_ARITH_PROBE=vmaf_test_hip_fp_arith -DFP_ARITH_DEVICE="HIP", against
 *  the probe in test_hip_fp_arith_probe.{hip,c}: one set of operands and one
 *  set of host references for both backends.
 *
 *  ADR-1488: psnr_hvs_sycl takes its masking threshold from sqrt_rn()
 *  (feature/sycl/sycl_exact_fp.h) of the fp32 product of the masking energy
 *  and the variance ratio. The CPU extractor writes upstream's statement,
 *  sqrt(s_mask * s_gvar) (Netflix/vmaf
 *  libvmaf/src/feature/third_party/xiph/psnr_hvs.c:316-317): the fp32 product,
 *  its root in fp64, rounded to fp32. The two are the same value for every
 *  product, and the fourth result checks it on the device.
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

/* The device probe under test and the device it names when it skips. */
#ifndef FP_ARITH_PROBE
#define FP_ARITH_PROBE vmaf_test_sycl_fp_arith
#define FP_ARITH_DEVICE "SYCL GPU"
#define FP_ARITH_HAS_PROD_ROOT 1
#else
#ifndef FP_ARITH_HAS_PROD_ROOT
#define FP_ARITH_HAS_PROD_ROOT 0
#endif
#endif

#if FP_ARITH_HAS_PROD_ROOT
int FP_ARITH_PROBE(const float *a, const float *b, const float *c, size_t n, float *mad,
                   float *quot, float *root, float *prod_root);
#else
int FP_ARITH_PROBE(const float *a, const float *b, const float *c, size_t n, float *mad,
                   float *quot, float *root);
#endif

enum {
    RANDOM_COUNT = 1 << 20,
    BOUNDARY_VALUES = 13,
    BOUNDARY_COUNT = BOUNDARY_VALUES * BOUNDARY_VALUES * BOUNDARY_VALUES,
    MIDPOINT_COUNT = 1 << 16,
    MIDPOINT_FORMS = 4,
    MIDPOINT_OPERANDS = MIDPOINT_COUNT * MIDPOINT_FORMS,
    RESULTS = 7,
};

typedef struct {
    float *a;
    float *b;
    float *c;
    float *mad;
    float *quot;
    float *root;
    float *prod_root;
    size_t n;
} Operands;

typedef struct {
    size_t mad;
    size_t quot;
    size_t root;
    size_t prod_root;
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
    Mismatches m = {0, 0, 0, 0};
    for (size_t i = 0; i < op->n; i++) {
        const float product = (float)((double)op->a[i] * (double)op->b[i]);
        const float mad = (float)((double)product + (double)op->c[i]);
        const float quot = (float)((double)op->a[i] / (double)op->b[i]);
        const float root = (float)sqrt((double)fabsf(op->a[i]));
        m.mad += !same_float(mad, op->mad[i]);
        m.quot += !same_float(quot, op->quot[i]);
        m.root += !same_float(root, op->root[i]);
#if FP_ARITH_HAS_PROD_ROOT
        /* calc_psnrhvs()'s threshold expression: the product rounded to
         * fp32, its root rounded to fp64 and then to fp32. */
        const float prod = fabsf(op->a[i]) * fabsf(op->b[i]);
        const float prod_root = (float)sqrt((double)prod);
        m.prod_root += !same_float(prod_root, op->prod_root[i]);
#endif
    }
    return m;
}

static int operands_alloc(Operands *op, size_t n)
{
    float *block = calloc(RESULTS * n, sizeof(float));
    if (!block)
        return -ENOMEM;
    *op = (Operands){.a = block,
                     .b = block + n,
                     .c = block + (2 * n),
                     .mad = block + (3 * n),
                     .quot = block + (4 * n),
                     .root = block + (5 * n),
                     .prod_root = block + (6 * n),
                     .n = n};
    return 0;
}

/* Runs the device probe; NULL when the check passed or was skipped. */
static char *run_and_check(Operands *op, const char *label)
{
#if FP_ARITH_HAS_PROD_ROOT
    const int err =
        FP_ARITH_PROBE(op->a, op->b, op->c, op->n, op->mad, op->quot, op->root, op->prod_root);
#else
    const int err = FP_ARITH_PROBE(op->a, op->b, op->c, op->n, op->mad, op->quot, op->root);
#endif
    if (err == -ENODEV) {
        (void)fprintf(stderr, "  [SKIP] %s: no " FP_ARITH_DEVICE " device\n", label);
        mu_skipped = 1;
        return NULL;
    }
    mu_assert("device probe failed", err == 0);
    const Mismatches m = count_mismatches(op);
#if FP_ARITH_HAS_PROD_ROOT
    (void)fprintf(stderr, "  %s: %zu operands, mismatches mad=%zu div=%zu sqrt=%zu prod_root=%zu\n",
                  label, op->n, m.mad, m.quot, m.root, m.prod_root);
#else
    (void)fprintf(stderr, "  %s: %zu operands, mismatches mad=%zu div=%zu sqrt=%zu\n", label, op->n,
                  m.mad, m.quot, m.root);
#endif
    mu_assert("a * b + c was contracted into an FMA on the device", m.mad == 0);
    mu_assert("device fp32 division is not correctly rounded", m.quot == 0);
    mu_assert("device fp32 sqrt is not correctly rounded", m.root == 0);
#if FP_ARITH_HAS_PROD_ROOT
    mu_assert("sqrt_rn() of the fp32 product differs from the host's fp32 product and fp64 root",
              m.prod_root == 0);
#endif
    return NULL;
}

static char *test_invalid_arguments(void)
{
    float x = 1.0f;
#if FP_ARITH_HAS_PROD_ROOT
    mu_assert("NULL operand must be rejected",
              FP_ARITH_PROBE(NULL, &x, &x, 1, &x, &x, &x, &x) == -EINVAL);
    mu_assert("NULL result must be rejected",
              FP_ARITH_PROBE(&x, &x, &x, 1, &x, &x, NULL, &x) == -EINVAL);
    mu_assert("NULL product root must be rejected",
              FP_ARITH_PROBE(&x, &x, &x, 1, &x, &x, &x, NULL) == -EINVAL);
    mu_assert("an empty probe is a no-op", FP_ARITH_PROBE(&x, &x, &x, 0, &x, &x, &x, &x) == 0);
#else
    mu_assert("NULL operand must be rejected",
              FP_ARITH_PROBE(NULL, &x, &x, 1, &x, &x, &x) == -EINVAL);
    mu_assert("NULL result must be rejected",
              FP_ARITH_PROBE(&x, &x, &x, 1, &x, &x, NULL) == -EINVAL);
    mu_assert("an empty probe is a no-op", FP_ARITH_PROBE(&x, &x, &x, 0, &x, &x, &x) == 0);
#endif
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

/* Products whose exact root lies nearest a rounding boundary. For a 24-bit
 * k, sqrt(k * (k + 1)) is 1 / (8 k) below k + 1/2, the midpoint of two fp32
 * values; k * k has an exact root, k * (k + 2) one just below k + 1, and the
 * doubled operand moves the product to an odd binary exponent. On these the
 * root of the fp32-rounded product, which the reference takes, and the root
 * of the exact product, which the twin took before ADR-1488, differ most
 * often, so a kernel that keeps the product exact fails here. */
static char *test_midpoint_operands(void)
{
    Operands op;
    mu_assert("operand allocation failed", operands_alloc(&op, MIDPOINT_OPERANDS) == 0);
    size_t n = 0;
    for (uint32_t i = 0; i < MIDPOINT_COUNT; i++) {
        const float k = (float)((1u << 23) + (i * 127u));
        const float partner[MIDPOINT_FORMS] = {k + 1.0f, k, k + 2.0f, 2.0f * (k + 1.0f)};
        for (size_t form = 0; form < MIDPOINT_FORMS; form++, n++) {
            op.a[n] = k;
            op.b[n] = partner[form];
            op.c[n] = 1.0f;
        }
    }
    char *msg = run_and_check(&op, "midpoint");
    free(op.a);
    return msg;
}

char *run_tests(void)
{
    mu_run_test(test_invalid_arguments);
    mu_run_test(test_random_operands);
    mu_run_test(test_boundary_operands);
    mu_run_test(test_midpoint_operands);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
