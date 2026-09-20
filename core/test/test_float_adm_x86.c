/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Bit-exact regression coverage for the x86 float-ADM CSF kernels.
 *
 * adm_csf_s() multiplies the filtered absolute value by the unsuffixed
 * FLOAT_ONE_BY_30 literal, so the multiply is evaluated in double precision
 * before the result is stored as float.  The AVX2 and AVX-512 implementations
 * must preserve that rounding contract; rounding the constant to float first
 * changes some lanes by one ULP and can change the public AIM/ADM3 scores.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "test.h"

#include "cpu.h"

#if ARCH_X86
#include "feature/x86/float_adm_avx2.h"
#if HAVE_AVX512
#include "feature/x86/float_adm_avx512.h"
#endif



#define CSF_W 33
#define CSF_H 3
#define CSF_SRC_STRIDE 40
#define CSF_DST_STRIDE 48
#define CSF_ONE_BY_30 0.0333333351

typedef void (*FloatAdmCsfFn)(const float *, float *, float *, int, int, int, int, float, double);

static float float_from_bits(uint32_t bits)
{
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t float_bits(float value)
{
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static int run_csf_case(FloatAdmCsfFn csf, const char *label)
{
    static const float factors[] = {1.0f, 0.10133042f, 3.7071068f};
    const size_t src_len = (size_t)CSF_H * CSF_SRC_STRIDE;
    const size_t dst_len = (size_t)CSF_H * CSF_DST_STRIDE;
    const float sentinel = float_from_bits(UINT32_C(0xC5A5A5A5));
    float *src = calloc(src_len, sizeof(*src));
    float *dst = malloc(dst_len * sizeof(*dst));
    float *flt = malloc(dst_len * sizeof(*flt));
    if (!src || !dst || !flt) {
        free(src);
        free(dst);
        free(flt);
        return 0;
    }

    for (size_t i = 0; i < src_len; ++i) {
        const uint32_t magnitude = UINT32_C(0x3E800000) + (uint32_t)(i * UINT32_C(7919));
        src[i] = float_from_bits(magnitude | ((i & 1u) ? UINT32_C(0x80000000) : 0u));
    }
    /* This exact lane distinguishes double-literal multiplication from a
     * prematurely rounded float constant:
     *   double literal -> 0x3c906f01, float constant -> 0x3c906f00. */
    src[0] = float_from_bits(UINT32_C(0x3F076810));

    int ok = 1;
    for (size_t f = 0; f < sizeof(factors) / sizeof(factors[0]); ++f) {
        for (size_t i = 0; i < dst_len; ++i) {
            dst[i] = sentinel;
            flt[i] = sentinel;
        }

        csf(src, dst, flt, CSF_W, CSF_H, CSF_SRC_STRIDE * (int)sizeof(float),
            CSF_DST_STRIDE * (int)sizeof(float), factors[f], CSF_ONE_BY_30);

        for (int row = 0; row < CSF_H; ++row) {
            for (int col = 0; col < CSF_W; ++col) {
                const size_t src_idx = (size_t)row * CSF_SRC_STRIDE + (size_t)col;
                const size_t dst_idx = (size_t)row * CSF_DST_STRIDE + (size_t)col;
                const float expected_dst = factors[f] * src[src_idx];
                const float expected_flt = CSF_ONE_BY_30 * fabsf(expected_dst);
                if (float_bits(dst[dst_idx]) != float_bits(expected_dst) ||
                    float_bits(flt[dst_idx]) != float_bits(expected_flt)) {
                    (void)fprintf(stderr,
                                  "%s CSF mismatch factor=%a row=%d col=%d: "
                                  "dst=%a expected=%a flt=%a expected=%a\n",
                                  label, (double)factors[f], row, col, (double)dst[dst_idx],
                                  (double)expected_dst, (double)flt[dst_idx],
                                  (double)expected_flt);
                    ok = 0;
                    goto out;
                }
            }
            for (int col = CSF_W; col < CSF_DST_STRIDE; ++col) {
                const size_t idx = (size_t)row * CSF_DST_STRIDE + (size_t)col;
                if (float_bits(dst[idx]) != float_bits(sentinel) ||
                    float_bits(flt[idx]) != float_bits(sentinel)) {
                    (void)fprintf(stderr, "%s CSF overwrote row padding at row=%d col=%d\n",
                                  label, row, col);
                    ok = 0;
                    goto out;
                }
            }
        }
    }

out:
    free(src);
    free(dst);
    free(flt);
    return ok;
}

static char *test_float_adm_csf_x86_matches_scalar(void)
{
    const unsigned flags = vmaf_get_cpu_flags();
    if (flags & VMAF_X86_CPU_FLAG_AVX2) {
        mu_assert("float_adm_csf_avx2 diverges from scalar rounding",
                  run_csf_case(float_adm_csf_avx2, "AVX2"));
    }
#if HAVE_AVX512
    if (flags & VMAF_X86_CPU_FLAG_AVX512) {
        mu_assert("float_adm_csf_avx512 diverges from scalar rounding",
                  run_csf_case(float_adm_csf_avx512, "AVX-512"));
    }
#endif
    return VMAF_NULLPTR;
}


#endif

char *run_tests(void)
{
#if ARCH_X86
    mu_run_test(test_float_adm_csf_x86_matches_scalar);
#endif
    return VMAF_NULLPTR;
}
