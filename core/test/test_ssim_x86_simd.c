/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 *
 * AVX2- and AVX-512-vs-scalar bit-exactness for the SSIM SIMD kernels in
 * `core/src/feature/x86/ssim_avx2.c` and `ssim_avx512.c`:
 * `ssim_precompute_*`, `ssim_variance_*` and `ssim_accumulate_*`. The x86
 * sibling of `test_ssim_neon.c`.
 *
 * Why this file exists (ADR-1415, T-ICX-SSIM-AVX512-FP-CONTRACT-2026-10-01): the
 * kernels finish the last `n % lanes` elements in plain C. `ssim_avx512.c`
 * was built without `-ffp-contract=off`, and under icx
 * (`-fp-model=precise`, which implies contraction) those tails became fused
 * multiply-adds: `sigma -= mu * mu` and `rm * rm + cm * cm + C1` rounded
 * once where the scalar reference in `iqa/ssim_tools.c` rounds twice. On an
 * AVX-512 host the `float_ms_ssim` and `float_ssim` CPU extractors of an icx
 * build (every SYCL build is one) therefore differed from the scalar path and
 * from a GCC build in the tail of each plane.
 *
 * The tests transcribe `ssim_precompute_scalar`, `ssim_variance_scalar` and
 * `ssim_accumulate_default_scalar` (all `static` in `iqa/ssim_tools.c`) and
 * demand bit-identical results over element counts that are and are not
 * multiples of the 8- and 16-lane strides. The inputs are random fp32
 * values, for which a fused and an unfused multiply-add differ on most
 * elements, so a contracted tail fails on the first non-multiple count.
 *
 * A kernel whose instruction set the host lacks is skipped.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "test.h"

#if ARCH_X86

#include "cpu.h"
#include "feature/x86/ssim_avx2.h"
#if HAVE_AVX512
#include "feature/x86/ssim_avx512.h"
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Every residue class of both strides, on both sides of one and two blocks. */
static const int k_sizes[] = {0,  1,  2,  3,  7,  8,  9,  15, 16, 17, 23,  24,
                              25, 31, 32, 33, 47, 48, 49, 63, 64, 65, 127, 129};
#define K_NUM_SIZES ((int)(sizeof(k_sizes) / sizeof(k_sizes[0])))
#define K_MAX_N 129

typedef struct SsimKernels {
    const char *name;
    unsigned cpu_flag;
    void (*precompute)(const float *ref, const float *cmp, float *ref_sq, float *cmp_sq,
                       float *ref_cmp, int n);
    void (*variance)(float *ref_sigma_sqd, float *cmp_sigma_sqd, float *sigma_both,
                     const float *ref_mu, const float *cmp_mu, int n);
    void (*accumulate)(const float *ref_mu, const float *cmp_mu, const float *ref_sigma_sqd,
                       const float *cmp_sigma_sqd, const float *sigma_both, int n, float C1,
                       float C2, float C3, double *ssim_sum, double *l_sum, double *c_sum,
                       double *s_sum);
} SsimKernels;

static const SsimKernels k_kernels[] = {
    {"avx2", VMAF_X86_CPU_FLAG_AVX2, ssim_precompute_avx2, ssim_variance_avx2,
     ssim_accumulate_avx2},
#if HAVE_AVX512
    {"avx512", VMAF_X86_CPU_FLAG_AVX512, ssim_precompute_avx512, ssim_variance_avx512,
     ssim_accumulate_avx512},
#endif
};
#define K_NUM_KERNELS ((int)(sizeof(k_kernels) / sizeof(k_kernels[0])))

/* The five planes the variance and accumulate stages read. */
typedef struct SsimPlanes {
    float ref_mu[K_MAX_N];
    float cmp_mu[K_MAX_N];
    float ref_sigma[K_MAX_N];
    float cmp_sigma[K_MAX_N];
    float sigma_both[K_MAX_N];
} SsimPlanes;

/* ssim_tools.c spells its clamp with this macro; the `0.0` double literal
 * (and the double round-trip of the float operand) is part of it. */
#define REF_MAX(x, y) (((x) > (y)) ? (x) : (y))

/* Transcribed from ssim_precompute_scalar() in iqa/ssim_tools.c. */
static void ref_precompute(const float *ref, const float *cmp, float *ref_sq, float *cmp_sq,
                           float *ref_cmp, int n)
{
    for (int i = 0; i < n; ++i) {
        ref_sq[i] = ref[i] * ref[i];
        cmp_sq[i] = cmp[i] * cmp[i];
        ref_cmp[i] = ref[i] * cmp[i];
    }
}

/* Transcribed from ssim_variance_scalar() in iqa/ssim_tools.c. */
static void ref_variance(SsimPlanes *p, int n)
{
    for (int i = 0; i < n; ++i) {
        p->ref_sigma[i] -= p->ref_mu[i] * p->ref_mu[i];
        p->cmp_sigma[i] -= p->cmp_mu[i] * p->cmp_mu[i];
        p->ref_sigma[i] = REF_MAX(0.0, p->ref_sigma[i]);
        p->cmp_sigma[i] = REF_MAX(0.0, p->cmp_sigma[i]);
        p->sigma_both[i] -= p->ref_mu[i] * p->cmp_mu[i];
    }
}

/* ssim, l, c, s sums of one accumulate call. */
typedef struct SsimSums {
    double ssim;
    double l;
    double c;
    double s;
} SsimSums;

/* Transcribed from ssim_accumulate_default_scalar() in iqa/ssim_tools.c. */
static SsimSums ref_accumulate(const SsimPlanes *p, int n, float C1, float C2, float C3)
{
    SsimSums sums = {0.0, 0.0, 0.0, 0.0};
    for (int i = 0; i < n; ++i) {
        const float sigma_ref_sigma_cmp = sqrtf(p->ref_sigma[i] * p->cmp_sigma[i]);
        const double l = (2.0 * p->ref_mu[i] * p->cmp_mu[i] + C1) /
                         (p->ref_mu[i] * p->ref_mu[i] + p->cmp_mu[i] * p->cmp_mu[i] + C1);
        const double c =
            (2.0 * sigma_ref_sigma_cmp + C2) / (p->ref_sigma[i] + p->cmp_sigma[i] + C2);
        const float clamped_sigma_both =
            (p->sigma_both[i] < 0.0f && sigma_ref_sigma_cmp <= 0.0f) ? 0.0f : p->sigma_both[i];
        const double s = (clamped_sigma_both + C3) / (sigma_ref_sigma_cmp + C3);
        sums.ssim += l * c * s;
        sums.l += l;
        sums.c += c;
        sums.s += s;
    }
    return sums;
}

static uint32_t rng_next(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Uniform in [-scale, scale), with all 24 significand bits in use. */
static float rng_float(uint32_t *state, float scale)
{
    const int32_t r = (int32_t)(rng_next(state) >> 8);
    return scale * ((float)r / 8388608.0f - 1.0f);
}

/* Window means near mid-grey and variances as a blurred picture has them. */
static void fill_planes(SsimPlanes *p, uint32_t seed)
{
    uint32_t state = seed;
    for (int i = 0; i < K_MAX_N; ++i) {
        p->ref_mu[i] = 128.0f + rng_float(&state, 127.0f);
        p->cmp_mu[i] = 128.0f + rng_float(&state, 127.0f);
        p->ref_sigma[i] = p->ref_mu[i] * p->ref_mu[i] + fabsf(rng_float(&state, 4096.0f));
        p->cmp_sigma[i] = p->cmp_mu[i] * p->cmp_mu[i] + fabsf(rng_float(&state, 4096.0f));
        p->sigma_both[i] = p->ref_mu[i] * p->cmp_mu[i] + rng_float(&state, 4096.0f);
    }
}

/* Number of elements of `a` and `b` whose bit patterns differ. */
static int count_f32_mismatches(const float *a, const float *b)
{
    int mismatches = 0;
    for (int i = 0; i < K_MAX_N; ++i) {
        uint32_t ua;
        uint32_t ub;
        memcpy(&ua, &a[i], sizeof(ua));
        memcpy(&ub, &b[i], sizeof(ub));
        mismatches += ua != ub;
    }
    return mismatches;
}

static int f64_bits_eq(double a, double b)
{
    uint64_t ua;
    uint64_t ub;
    memcpy(&ua, &a, sizeof(ua));
    memcpy(&ub, &b, sizeof(ub));
    return ua == ub;
}

static int kernel_available(const SsimKernels *k)
{
    /* The probe itself: vmaf_get_cpu_flags() is 0 until vmaf_init_cpu() runs. */
    if (vmaf_get_cpu_flags_x86() & k->cpu_flag)
        return 1;
    (void)fprintf(stderr, "[skip: no %s] ", k->name);
    return 0;
}

static char *check_precompute(const SsimKernels *k, int n)
{
    SsimPlanes in;
    SsimPlanes expected;
    SsimPlanes got;
    fill_planes(&in, 0x5eed0001u + (uint32_t)n);
    memset(&expected, 0x5a, sizeof(expected));
    memcpy(&got, &expected, sizeof(got));

    ref_precompute(in.ref_mu, in.cmp_mu, expected.ref_sigma, expected.cmp_sigma,
                   expected.sigma_both, n);
    k->precompute(in.ref_mu, in.cmp_mu, got.ref_sigma, got.cmp_sigma, got.sigma_both, n);

    const int mismatches = count_f32_mismatches(expected.ref_sigma, got.ref_sigma) +
                           count_f32_mismatches(expected.cmp_sigma, got.cmp_sigma) +
                           count_f32_mismatches(expected.sigma_both, got.sigma_both);
    if (mismatches) {
        (void)fprintf(stderr, "\n  ssim_precompute_%s n=%d: %d mismatching element(s)\n", k->name,
                      n, mismatches);
    }
    mu_assert("an x86 ssim_precompute kernel diverges from the scalar reference", !mismatches);
    return NULL;
}

static char *check_variance(const SsimKernels *k, int n)
{
    SsimPlanes expected;
    SsimPlanes got;
    fill_planes(&expected, 0x5eed0002u + (uint32_t)n);
    memcpy(&got, &expected, sizeof(got));

    ref_variance(&expected, n);
    k->variance(got.ref_sigma, got.cmp_sigma, got.sigma_both, got.ref_mu, got.cmp_mu, n);

    const int mismatches = count_f32_mismatches(expected.ref_sigma, got.ref_sigma) +
                           count_f32_mismatches(expected.cmp_sigma, got.cmp_sigma) +
                           count_f32_mismatches(expected.sigma_both, got.sigma_both);
    if (mismatches) {
        (void)fprintf(stderr, "\n  ssim_variance_%s n=%d: %d mismatching element(s)\n", k->name, n,
                      mismatches);
    }
    mu_assert("an x86 ssim_variance kernel diverges from the scalar reference", !mismatches);
    return NULL;
}

static char *check_accumulate(const SsimKernels *k, int n)
{
    /* Production constants: L = 255, K1 = 0.01, K2 = 0.03 (ssim_tools.c). */
    const float C1 = (0.01f * 255) * (0.01f * 255);
    const float C2 = (0.03f * 255) * (0.03f * 255);
    const float C3 = C2 / 2.0f;
    SsimPlanes p;
    fill_planes(&p, 0x5eed0003u + (uint32_t)n);
    /* The accumulate stage reads variances, not raw second moments. */
    ref_variance(&p, K_MAX_N);

    const SsimSums expected = ref_accumulate(&p, n, C1, C2, C3);
    SsimSums got = {0.0, 0.0, 0.0, 0.0};
    k->accumulate(p.ref_mu, p.cmp_mu, p.ref_sigma, p.cmp_sigma, p.sigma_both, n, C1, C2, C3,
                  &got.ssim, &got.l, &got.c, &got.s);

    const int same = f64_bits_eq(expected.ssim, got.ssim) && f64_bits_eq(expected.l, got.l) &&
                     f64_bits_eq(expected.c, got.c) && f64_bits_eq(expected.s, got.s);
    if (!same) {
        (void)fprintf(stderr,
                      "\n  ssim_accumulate_%s n=%d: ssim %a/%a  l %a/%a  c %a/%a  s %a/%a "
                      "(scalar/simd)\n",
                      k->name, n, expected.ssim, got.ssim, expected.l, got.l, expected.c, got.c,
                      expected.s, got.s);
    }
    mu_assert("an x86 ssim_accumulate kernel diverges from the scalar reference", same);
    return NULL;
}

/* Run `check` on every available kernel set at every element count. */
static char *for_each_kernel_and_size(char *(*check)(const SsimKernels *, int))
{
    for (int k = 0; k < K_NUM_KERNELS; ++k) {
        if (!kernel_available(&k_kernels[k]))
            continue;
        for (int t = 0; t < K_NUM_SIZES; ++t) {
            char *msg = check(&k_kernels[k], k_sizes[t]);
            if (msg)
                return msg;
        }
    }
    return NULL;
}

static char *test_ssim_precompute_matches_scalar(void)
{
    return for_each_kernel_and_size(check_precompute);
}

static char *test_ssim_variance_matches_scalar(void)
{
    return for_each_kernel_and_size(check_variance);
}

static char *test_ssim_accumulate_matches_scalar(void)
{
    return for_each_kernel_and_size(check_accumulate);
}

char *run_tests(void)
{
    mu_run_test(test_ssim_precompute_matches_scalar);
    mu_run_test(test_ssim_variance_matches_scalar);
    mu_run_test(test_ssim_accumulate_matches_scalar);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */

#else /* !ARCH_X86 */

char *run_tests(void)
{
    (void)fprintf(stderr, "[skip: not x86] ");
    return NULL;
}

#endif /* ARCH_X86 */
