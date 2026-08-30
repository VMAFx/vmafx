/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * NEON-vs-scalar bit-exactness for the three SSIM SIMD kernels in
 * `core/src/feature/arm64/ssim_neon.c` — `ssim_precompute_neon`,
 * `ssim_variance_neon` and `ssim_accumulate_neon`.
 *
 * Why this file exists: the `arm64_ssim_neon_lib` carve-out in
 * `core/src/meson.build` says in so many words "no parity test exists
 * yet", and the same is true of the AVX2 sibling. The kernels are
 * installed unconditionally on aarch64 (`float_ssim.c` /
 * `float_ms_ssim.c` gate only on `VMAF_ARM_CPU_FLAG_NEON`), with no
 * width predicate at all, so every element count — including the ones
 * the 4-wide vector body cannot cover — reaches them in production.
 * That makes the SIMD-body / scalar-tail boundary the interesting
 * region, exactly as in ADR-1057's `adm_dwt2_8_neon`.
 *
 * The tests transcribe `ssim_precompute_scalar`,
 * `ssim_variance_scalar` and `ssim_accumulate_default_scalar` from
 * `iqa/ssim_tools.c` (all three are `static` there and therefore
 * unlinkable) and demand *bit-identical* results — raw IEEE-754 bit
 * patterns, not a tolerance — over element counts that are and are not
 * multiples of the 4-lane NEON stride. The last test closes the loop
 * end-to-end through the real, non-transcribed `iqa_ssim()` so the
 * production scalar object file itself is held to the same contract
 * (this is what would catch an FP-contraction regression in
 * `ssim_tools.c`, which is *not* built with `-ffp-contract=off`).
 *
 * Found by this test: `ssim_variance_neon`'s vector body clamped with
 * `vmaxq_f32(x, 0)`. FMAX forces a zero result positive (ARM ARM
 * `FPMax`: `if type == FPType_Zero then sign = sign1 AND sign2`),
 * whereas the scalar reference's `MAX(0.0, x)` — and this same
 * kernel's own scalar tail — return `x` unchanged for `x == -0.0f`.
 * The vector body therefore disagreed both with the reference and with
 * its own tail on the identical input. See the report in the ADR /
 * changelog fragment for the (nil) score impact.
 */

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "test.h"

#if ARCH_AARCH64

#include "feature/arm64/ssim_neon.h"
#include "feature/iqa/ssim_simd.h"
#include "feature/iqa/ssim_tools.h"

/* Element counts exercised by every kernel test. The 4-lane NEON body
 * covers `n - (n % 4)` elements and the scalar tail the rest, so the
 * list deliberately hits every residue class mod 4 (and n == 0). */
static const int k_sizes[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
                              15, 16, 17, 18, 31, 32, 33, 34, 63, 64, 65, 129};
#define K_NUM_SIZES ((int)(sizeof(k_sizes) / sizeof(k_sizes[0])))
#define K_MAX_N 129

/* ---------------------------------------------------------------- *
 * Scalar reference, transcribed from iqa/ssim_tools.c.
 * ---------------------------------------------------------------- */

/* ssim_tools.c spells its clamp with this macro; keep the spelling so
 * the `0.0` double literal (and hence the double round-trip of the
 * float operand) is reproduced exactly. */
#define REF_MAX(x, y) (((x) > (y)) ? (x) : (y))

/* Transcribed from ssim_precompute_scalar() in iqa/ssim_tools.c. */
static void ref_precompute(const float *ref, const float *cmp, float *ref_sq, float *cmp_sq,
                           float *ref_cmp, int w, int h)
{
    for (int y = 0; y < h; ++y) {
        int offset = y * w;
        for (int x = 0; x < w; ++x, ++offset) {
            ref_sq[offset] = ref[offset] * ref[offset];
            cmp_sq[offset] = cmp[offset] * cmp[offset];
            ref_cmp[offset] = ref[offset] * cmp[offset];
        }
    }
}

/* Transcribed from ssim_variance_scalar() in iqa/ssim_tools.c. */
static void ref_variance(float *ref_sigma_sqd, float *cmp_sigma_sqd, float *sigma_both,
                         const float *ref_mu, const float *cmp_mu, int w, int h)
{
    for (int y = 0; y < h; ++y) {
        int offset = y * w;
        for (int x = 0; x < w; ++x, ++offset) {
            ref_sigma_sqd[offset] -= ref_mu[offset] * ref_mu[offset];
            cmp_sigma_sqd[offset] -= cmp_mu[offset] * cmp_mu[offset];
            ref_sigma_sqd[offset] = REF_MAX(0.0, ref_sigma_sqd[offset]);
            cmp_sigma_sqd[offset] = REF_MAX(0.0, cmp_sigma_sqd[offset]);
            sigma_both[offset] -= ref_mu[offset] * cmp_mu[offset];
        }
    }
}

/* Transcribed from ssim_accumulate_default_scalar() in iqa/ssim_tools.c. */
static void ref_accumulate(const float *ref_mu, const float *cmp_mu, const float *ref_sigma_sqd,
                           const float *cmp_sigma_sqd, const float *sigma_both, int w, int h,
                           float C1, float C2, float C3, double *ssim_sum, double *l_sum,
                           double *c_sum, double *s_sum)
{
    for (int y = 0; y < h; ++y) {
        int offset = y * w;
        for (int x = 0; x < w; ++x, ++offset) {
            const float sigma_ref_sigma_cmp = sqrtf(ref_sigma_sqd[offset] * cmp_sigma_sqd[offset]);
            const double l =
                (2.0 * ref_mu[offset] * cmp_mu[offset] + C1) /
                (ref_mu[offset] * ref_mu[offset] + cmp_mu[offset] * cmp_mu[offset] + C1);
            const double c = (2.0 * sigma_ref_sigma_cmp + C2) /
                             (ref_sigma_sqd[offset] + cmp_sigma_sqd[offset] + C2);
            const float clamped_sigma_both =
                (sigma_both[offset] < 0.0f && sigma_ref_sigma_cmp <= 0.0f) ? 0.0f :
                                                                             sigma_both[offset];
            const double s = (clamped_sigma_both + C3) / (sigma_ref_sigma_cmp + C3);
            *ssim_sum += l * c * s;
            *l_sum += l;
            *c_sum += c;
            *s_sum += s;
        }
    }
}

/* ---------------------------------------------------------------- *
 * Helpers.
 * ---------------------------------------------------------------- */

/* Bit-pattern comparison: `==` would call -0.0f equal to +0.0f and
 * unequal to itself for NaN, both of which are exactly the cases this
 * test has to be able to see. */
static int f32_bits_eq(float a, float b)
{
    uint32_t ua;
    uint32_t ub;
    memcpy(&ua, &a, sizeof(ua));
    memcpy(&ub, &b, sizeof(ub));
    return ua == ub;
}

static int f64_bits_eq(double a, double b)
{
    uint64_t ua;
    uint64_t ub;
    memcpy(&ua, &a, sizeof(ua));
    memcpy(&ub, &b, sizeof(ub));
    return ua == ub;
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

/* Uniform in [-scale, scale). */
static float rng_float(uint32_t *state, float scale)
{
    const int32_t r = (int32_t)(rng_next(state) >> 8); /* 24-bit */
    return scale * ((float)r / 8388608.0f - 1.0f);
}

/* Value generator that also emits the FP classes a plain uniform draw
 * would essentially never produce: signed zeros, subnormals, and
 * magnitudes whose square overflows to infinity. */
static float rng_float_exotic(uint32_t *state, float scale)
{
    const uint32_t sel = rng_next(state) % 24u;
    switch (sel) {
    case 0:
        return 0.0f;
    case 1:
        return -0.0f;
    case 2:
        return FLT_TRUE_MIN;
    case 3:
        return -FLT_TRUE_MIN;
    case 4:
        return FLT_MIN;
    case 5:
        return 1.0e20f; /* squares to +inf */
    case 6:
        return -1.0e20f;
    default:
        return rng_float(state, scale);
    }
}

/* ---------------------------------------------------------------- *
 * Tests.
 * ---------------------------------------------------------------- */

static char *check_precompute_case(int n, uint32_t seed)
{
    float ref[K_MAX_N];
    float cmp[K_MAX_N];
    float exp_rs[K_MAX_N];
    float exp_cs[K_MAX_N];
    float exp_sb[K_MAX_N];
    float got_rs[K_MAX_N];
    float got_cs[K_MAX_N];
    float got_sb[K_MAX_N];
    uint32_t state = seed;

    for (int i = 0; i < K_MAX_N; ++i) {
        ref[i] = rng_float_exotic(&state, 256.0f);
        cmp[i] = rng_float_exotic(&state, 256.0f);
    }
    memset(exp_rs, 0x5a, sizeof(exp_rs));
    memset(exp_cs, 0x5a, sizeof(exp_cs));
    memset(exp_sb, 0x5a, sizeof(exp_sb));
    memcpy(got_rs, exp_rs, sizeof(got_rs));
    memcpy(got_cs, exp_cs, sizeof(got_cs));
    memcpy(got_sb, exp_sb, sizeof(got_sb));

    ref_precompute(ref, cmp, exp_rs, exp_cs, exp_sb, n, 1);
    ssim_precompute_neon(ref, cmp, got_rs, got_cs, got_sb, n);

    int mismatches = 0;
    for (int i = 0; i < K_MAX_N; ++i) {
        const int in_body = (i < n - (n % 4));
        if (!f32_bits_eq(exp_rs[i], got_rs[i]) || !f32_bits_eq(exp_cs[i], got_cs[i]) ||
            !f32_bits_eq(exp_sb[i], got_sb[i])) {
            ++mismatches;
            (void)fprintf(stderr,
                          "\n  n=%d i=%d (%s): ref_sq %a/%a  cmp_sq %a/%a  ref_cmp %a/%a "
                          "(scalar/neon)",
                          n, i, (i >= n) ? "past end" : (in_body ? "vector body" : "scalar tail"),
                          (double)exp_rs[i], (double)got_rs[i], (double)exp_cs[i],
                          (double)got_cs[i], (double)exp_sb[i], (double)got_sb[i]);
        }
    }
    if (mismatches) {
        (void)fprintf(stderr, "\n  n=%d: %d mismatching element(s)\n", n, mismatches);
        mu_assert("ssim_precompute_neon diverges from the scalar reference", 0);
    }
    return NULL;
}

static char *test_ssim_precompute_neon_matches_scalar(void)
{
    for (int t = 0; t < K_NUM_SIZES; ++t) {
        char *msg = check_precompute_case(k_sizes[t], 0x5eed0001u + (uint32_t)k_sizes[t]);
        if (msg)
            return msg;
    }
    return NULL;
}

/* Seeds the variance inputs. Index 0 and index `n - 1` (when it exists)
 * are forced to the exact case that separates FMAX from the scalar
 * clamp: `sigma - mu * mu` evaluating to -0.0f. Putting it at both ends
 * means it lands inside the vector body for some `n` and inside the
 * scalar tail for others. */
static void fill_variance_inputs(int n, uint32_t seed, float *rs, float *cs, float *sb, float *rm,
                                 float *cm)
{
    uint32_t state = seed;

    for (int i = 0; i < K_MAX_N; ++i) {
        rm[i] = rng_float(&state, 64.0f);
        cm[i] = rng_float(&state, 64.0f);
        /* Deliberately straddles rm*rm so the clamp fires for roughly
         * half the elements. */
        rs[i] = rm[i] * rm[i] + rng_float(&state, 512.0f);
        cs[i] = cm[i] * cm[i] + rng_float(&state, 512.0f);
        sb[i] = rng_float(&state, 512.0f);
    }
    for (int i = 0; i < K_MAX_N; ++i) {
        if (i != 0 && i != n - 1)
            continue;
        rm[i] = 0.0f;
        cm[i] = 0.0f;
        rs[i] = -0.0f; /* -0.0f - 0.0f*0.0f == -0.0f */
        cs[i] = -0.0f;
        sb[i] = -0.0f;
    }
}

static char *check_variance_case(int n, uint32_t seed)
{
    float rm[K_MAX_N];
    float cm[K_MAX_N];
    float exp_rs[K_MAX_N];
    float exp_cs[K_MAX_N];
    float exp_sb[K_MAX_N];
    float got_rs[K_MAX_N];
    float got_cs[K_MAX_N];
    float got_sb[K_MAX_N];

    fill_variance_inputs(n, seed, exp_rs, exp_cs, exp_sb, rm, cm);
    memcpy(got_rs, exp_rs, sizeof(got_rs));
    memcpy(got_cs, exp_cs, sizeof(got_cs));
    memcpy(got_sb, exp_sb, sizeof(got_sb));

    ref_variance(exp_rs, exp_cs, exp_sb, rm, cm, n, 1);
    ssim_variance_neon(got_rs, got_cs, got_sb, rm, cm, n);

    int mismatches = 0;
    for (int i = 0; i < K_MAX_N; ++i) {
        const int in_body = (i < n - (n % 4));
        if (!f32_bits_eq(exp_rs[i], got_rs[i]) || !f32_bits_eq(exp_cs[i], got_cs[i]) ||
            !f32_bits_eq(exp_sb[i], got_sb[i])) {
            ++mismatches;
            (void)fprintf(stderr,
                          "\n  n=%d i=%d (%s): ref_sigma %a/%a  cmp_sigma %a/%a  sigma_both "
                          "%a/%a (scalar/neon)",
                          n, i, (i >= n) ? "past end" : (in_body ? "vector body" : "scalar tail"),
                          (double)exp_rs[i], (double)got_rs[i], (double)exp_cs[i],
                          (double)got_cs[i], (double)exp_sb[i], (double)got_sb[i]);
        }
    }
    if (mismatches) {
        (void)fprintf(stderr, "\n  n=%d: %d mismatching element(s)\n", n, mismatches);
        mu_assert("ssim_variance_neon diverges from the scalar reference", 0);
    }
    return NULL;
}

static char *test_ssim_variance_neon_matches_scalar(void)
{
    /* Every size is run even after the first failure so the diagnostic
     * shows the full mismatch census (which residues of n break, and
     * whether the offending index sits in the vector body or the tail)
     * rather than only the first offender. */
    char *first = NULL;
    for (int t = 0; t < K_NUM_SIZES; ++t) {
        char *msg = check_variance_case(k_sizes[t], 0x5eed0002u + (uint32_t)k_sizes[t]);
        if (msg && !first)
            first = msg;
    }
    return first;
}

/* Accumulate inputs stay inside the domain the production pipeline
 * guarantees (sigma_sqd >= 0, everything finite) — `sqrtf` of a
 * negative would only test that both paths agree on NaN. The directed
 * entries cover the flat-region clamp (`sigma_both < 0` with
 * `srsc <= 0`), exact zero variance on one side only, and a perfectly
 * identical pair. */
static void fill_accumulate_inputs(uint32_t seed, float *rm, float *cm, float *rs, float *cs,
                                   float *sb)
{
    uint32_t state = seed;

    for (int i = 0; i < K_MAX_N; ++i) {
        rm[i] = 128.0f + rng_float(&state, 127.0f);
        cm[i] = 128.0f + rng_float(&state, 127.0f);
        rs[i] = fabsf(rng_float(&state, 4096.0f));
        cs[i] = fabsf(rng_float(&state, 4096.0f));
        sb[i] = rng_float(&state, 4096.0f);
    }
    for (int i = 0; i + 6 <= K_MAX_N; i += 7) {
        /* flat region, sigma_both nudged negative by rounding */
        rm[i] = 16.0f;
        cm[i] = 16.0f;
        rs[i] = 0.0f;
        cs[i] = 0.0f;
        sb[i] = -1.0e-30f;
        /* flat region, sigma_both exactly zero */
        rm[i + 1] = 0.0f;
        cm[i + 1] = 0.0f;
        rs[i + 1] = 0.0f;
        cs[i + 1] = 0.0f;
        sb[i + 1] = 0.0f;
        /* zero variance on the reference side only */
        rs[i + 2] = 0.0f;
        cs[i + 2] = 900.0f;
        sb[i + 2] = -3.5f;
        /* negative sigma_both with a non-zero srsc: clamp must NOT fire */
        rs[i + 3] = 4.0f;
        cs[i + 3] = 9.0f;
        sb[i + 3] = -5.0f;
        /* identical pictures */
        rm[i + 4] = 200.0f;
        cm[i + 4] = 200.0f;
        rs[i + 4] = 1234.5f;
        cs[i + 4] = 1234.5f;
        sb[i + 4] = 1234.5f;
        /* subnormal variance */
        rs[i + 5] = FLT_TRUE_MIN;
        cs[i + 5] = FLT_TRUE_MIN;
        sb[i + 5] = -FLT_TRUE_MIN;
    }
}

static char *check_accumulate_case(int n, uint32_t seed)
{
    /* Production constants: L = 255, K1 = 0.01, K2 = 0.03 (ssim_tools.c). */
    const float C1 = (0.01f * 255) * (0.01f * 255);
    const float C2 = (0.03f * 255) * (0.03f * 255);
    const float C3 = C2 / 2.0f;
    float rm[K_MAX_N];
    float cm[K_MAX_N];
    float rs[K_MAX_N];
    float cs[K_MAX_N];
    float sb[K_MAX_N];
    double exp_ssim = 0.0;
    double exp_l = 0.0;
    double exp_c = 0.0;
    double exp_s = 0.0;
    double got_ssim = 0.0;
    double got_l = 0.0;
    double got_c = 0.0;
    double got_s = 0.0;

    fill_accumulate_inputs(seed, rm, cm, rs, cs, sb);

    ref_accumulate(rm, cm, rs, cs, sb, n, 1, C1, C2, C3, &exp_ssim, &exp_l, &exp_c, &exp_s);
    ssim_accumulate_neon(rm, cm, rs, cs, sb, n, C1, C2, C3, &got_ssim, &got_l, &got_c, &got_s);

    if (!f64_bits_eq(exp_ssim, got_ssim) || !f64_bits_eq(exp_l, got_l) ||
        !f64_bits_eq(exp_c, got_c) || !f64_bits_eq(exp_s, got_s)) {
        (void)fprintf(stderr,
                      "\n  n=%d (body %d + tail %d): ssim %a/%a  l %a/%a  c %a/%a  s %a/%a "
                      "(scalar/neon)\n",
                      n, n - (n % 4), n % 4, exp_ssim, got_ssim, exp_l, got_l, exp_c, got_c, exp_s,
                      got_s);
        mu_assert("ssim_accumulate_neon diverges from the scalar reference", 0);
    }
    return NULL;
}

static char *test_ssim_accumulate_neon_matches_scalar(void)
{
    for (int t = 0; t < K_NUM_SIZES; ++t) {
        char *msg = check_accumulate_case(k_sizes[t], 0x5eed0003u + (uint32_t)k_sizes[t]);
        if (msg)
            return msg;
    }
    return NULL;
}

/* End-to-end: the same picture pair through the real `iqa_ssim()` with
 * the NEON dispatch installed and with no dispatch at all. Unlike the
 * three kernel tests above, this one runs against the *compiled*
 * `iqa/ssim_tools.c` rather than a transcription, so it also pins the
 * scalar TU's own floating-point codegen (`ssim_tools.c` is built
 * without `-ffp-contract=off`, unlike `arm64_ssim_neon_lib`).
 * `iqa_convolve` is left on its scalar path for both legs so the only
 * variable is the SSIM kernel triple. */
static char *test_ssim_neon_end_to_end_matches_scalar(void)
{
    /* 67 x 43 so w*h is odd and every intermediate element count that
     * reaches the kernels has a non-empty 4-lane tail. */
    const int w = 67;
    const int h = 43;
    const size_t n = (size_t)w * (size_t)h;
    struct iqa_kernel window;
    float *ref = malloc(n * sizeof(float));
    float *cmp = malloc(n * sizeof(float));
    float scalar_score;
    float neon_score;
    float scalar_l;
    float scalar_c;
    float scalar_s;
    float neon_l;
    float neon_c;
    float neon_s;
    uint32_t state = 0x5eed0004u;
    char *msg = NULL;

    mu_assert("out of memory", ref && cmp);

    window.kernel = (float *)g_gaussian_window;
    window.kernel_h = (float *)g_gaussian_window_h;
    window.kernel_v = (float *)g_gaussian_window_v;
    window.w = GAUSSIAN_LEN;
    window.h = GAUSSIAN_LEN;
    window.normalized = 1;
    window.bnd_opt = KBND_SYMMETRIC;
    window.bnd_const = 0.0f;

    for (size_t i = 0; i < n; ++i) {
        ref[i] = 128.0f + rng_float(&state, 120.0f);
        /* A flat run so the zero-variance / clamp branch is reached. */
        cmp[i] = (i % 37u < 11u) ? ref[i] : ref[i] + rng_float(&state, 24.0f);
    }

    iqa_convolve_set_dispatch(NULL);
    iqa_ssim_set_dispatch(NULL, NULL, NULL);
    scalar_score = iqa_ssim(ref, cmp, w, h, &window, NULL, NULL, &scalar_l, &scalar_c, &scalar_s);

    iqa_ssim_set_dispatch(ssim_precompute_neon, ssim_variance_neon, ssim_accumulate_neon);
    neon_score = iqa_ssim(ref, cmp, w, h, &window, NULL, NULL, &neon_l, &neon_c, &neon_s);

    /* Leave the globals as the rest of the process expects them. */
    iqa_ssim_set_dispatch(NULL, NULL, NULL);

    if (!f32_bits_eq(scalar_score, neon_score) || !f32_bits_eq(scalar_l, neon_l) ||
        !f32_bits_eq(scalar_c, neon_c) || !f32_bits_eq(scalar_s, neon_s)) {
        (void)fprintf(stderr,
                      "\n  %dx%d: ssim %.9g/%.9g  l %.9g/%.9g  c %.9g/%.9g  s %.9g/%.9g "
                      "(scalar/neon)\n",
                      w, h, (double)scalar_score, (double)neon_score, (double)scalar_l,
                      (double)neon_l, (double)scalar_c, (double)neon_c, (double)scalar_s,
                      (double)neon_s);
        msg = "iqa_ssim with the NEON dispatch diverges from the scalar path";
    }

    free(ref);
    free(cmp);
    return msg;
}

#endif /* ARCH_AARCH64 */

char *run_tests(void)
{
#if ARCH_AARCH64
    mu_run_test(test_ssim_precompute_neon_matches_scalar);
    mu_run_test(test_ssim_variance_neon_matches_scalar);
    mu_run_test(test_ssim_accumulate_neon_matches_scalar);
    mu_run_test(test_ssim_neon_end_to_end_matches_scalar);
#else
    (void)fprintf(stderr, "skipping: non-aarch64 arch\n");
#endif
    return NULL;
}
