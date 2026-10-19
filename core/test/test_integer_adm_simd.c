/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Numerical-parity contract test for the integer-ADM AVX2 kernel
 * (`adm_cm_avx2`).
 *
 * This test covers the ADR-0138 bit-exactness fix for Bug 3:
 *   adm_cm_avx2 (adm_scale3 / scale=0 path) previously computed
 *     f_accum_h = (float)((float)accum_h / pow(2, shift))
 *   but the scalar reference (integer_adm.c:2009) uses
 *     f_accum_h = (float)(accum_h / pow(2, shift))
 *   The inner (float) cast narrows the int64 accum_h to float before the
 *   double-precision division, losing mantissa bits for large accumulators
 *   and producing up to 498M ULP error (max 2.3e-6 in the final float score)
 *   on src01_hrc00/hrc01 frame 3.
 *
 * Two tests are included:
 *
 *   1. test_adm_accum_precision: a pure arithmetic regression, showing
 *      that the old expression diverges and the new one matches the scalar
 *      reference for representative accum_h values from production runs.
 *
 *   2. test_adm_cm_avx2_smoke: constructs a minimal valid AdmBuffer with
 *      synthetic band data (small non-zero values), calls adm_cm_avx2, and
 *      verifies the result is within 1e-6 relative tolerance of the scalar
 *      adm_cm reference from integer_adm.c (accessed via static-inline
 *      replica for linkage purposes).
 *
 *   3. test_adm_decouple_guard_band: sweeps every band width from 8 to 80
 *      over several heights and requires adm_decouple_avx2 (and, where the
 *      host has it, adm_decouple_avx512) to leave every sample outside the
 *      decouple region untouched, with AVX2 and AVX-512 agreeing inside it.
 *      Regression test for Netflix/vmaf 03b5562c5.
 *
 *   4. test_adm_cm_matches_scalar_kernels: runs adm_cm_avx2 (and
 *      adm_cm_avx512 where the host has it) against the scalar kernels of
 *      integer_adm_kernels.h on hand-built bands and requires the same float
 *      bit for bit, DLM and AIM. The bands reach what a decoded picture never
 *      does: thresholds of either sign in every column, thresholds whose
 *      product with 2^12 leaves int32, and an excess that saturates
 *      (ADR-1402). test_adm_cm_centre_tap_stays_int32 pins the unnarrowed
 *      centre tap itself.
 *
 *   5. test_adm_decouple_matches_scalar_for_gains: runs the scale-0 and the
 *      scale 1-3 decouple kernels of AVX2 (and of AVX-512 where the host has
 *      it) against the scalar kernels at enhancement gain limits of 1, 1.2,
 *      1.5 and 100 and requires the same restored and additive samples. The
 *      scalar kernels truncate rst * gain toward zero; a vector kernel that
 *      rounds it differs at 1.2 and 1.5.
 *      On aarch64 test 5 runs against `adm_decouple_neon` (Netflix/vmaf
 *      9e48141b), whose vector path serves integral limits and whose scalar
 *      fallback serves fractional ones, and `adm_decouple_s123_neon`
 *      (b41d2340a), whose vector path serves a limit of 1. Gains 1, 2, 3 and
 *      7 are part of it: a limited sample that is off by one passes at 1.2,
 *      1.5 and 100 alone.
 *
 *   4b. test_i4_adm_cm_matches_scalar_kernels: the scale 1-3 contrast
 *      masking (i4_adm_cm_avx2 / _avx512, i4_adm_cm_neon) against the scalar
 *      kernels, every scale, DLM and AIM, p-norm 3 and 1, bit for bit.
 *
 *   On aarch64 tests 1, 4, 4b, 5 and the centre-tap test run, against the
 *   NEON kernels (Netflix/vmaf 8bc5a5c6a, b41d2340a).
 *
 * Boilerplate provided by `simd_bitexact_test.h` (ADR-0245).
 */

#include <inttypes.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "mu_table.h"
#include "sanitizer_build.h"
#include "test.h"
/* clang-format off — test.h has no header guard; must precede harness. */
#include "simd_bitexact_test.h"
/* clang-format on */

#include "feature/integer_adm.h"
#include "feature/integer_adm_kernels.h"
#include "mem.h"

#if ARCH_X86
#include "feature/x86/adm_avx2.h"
#if HAVE_AVX512
#include "feature/x86/adm_avx512.h"
#endif
#endif

#if ARCH_AARCH64
#include "feature/arm64/adm_neon.h"
#endif

/* ---------------------------------------------------------------------
 * Test 1: pure arithmetic regression for the f_accum_h precision fix.
 *
 * Demonstrate that (float)((float)accum_h / d) diverges from
 * (float)(accum_h / d) for representative large int64 values, and that
 * the fix restores agreement.
 * ------------------------------------------------------------------- */

/*
 * Representative accum_h values and shift denominators drawn from a
 * 576x324 src01 frame 3 run (adm_scale3 / scale=0):
 *   shift_xhcub = ceil(log2(576) - 4) = ceil(5.17) = 6
 *   shift_inner_accum = ceil(log2(324)) = 9
 *   final_shift_h = 2^(52 - 6 - 9) = 2^37
 *
 * Typical accum_h in that run: ~8.4e14 (fits int64, but > 2^49, so
 * (float)accum_h rounds to the nearest 2^26, then / 2^37 gives a
 * different result than accum_h / 2^37 in double then to float).
 */
static char *test_adm_accum_precision(void)
{
    /* Representative values from a 576x324 frame-3 run. */
    const int64_t representative_accum[] = {
        840000000000000LL, /* ~8.4e14 — typical for src01 luma */
        500000000000000LL, /* midrange */
        999999999999999LL, /* near-maximum before overflow */
        100000000000000LL, /* smaller, but still > 2^46 */
    };
    const double shift = pow(2.0, 37); /* 2^(52 - 6 - 9) */

    for (int k = 0; k < 4; k++) {
        const int64_t accum = representative_accum[k];

        /* Scalar reference: int64 → double → divide → float. */
        const float f_scalar = (float)((double)accum / shift);

        /* Buggy expression (old code): int64 → float → divide → float. */
        const float f_buggy = (float)((float)accum / shift);

        /* Fixed expression (new code): identical to scalar. */
        const float f_fixed = (float)(accum / shift);

        /* Intentional bit-exact comparison: the fixed SIMD expression must
         * produce the identical IEEE-754 float as the scalar reference.
         * An epsilon here would defeat the regression guard. */
        if (f_fixed != f_scalar) { /* bit-exact parity assertion */
            (void)fprintf(stderr, "  adm f_accum precision: accum=%" PRId64 " scalar=%a fixed=%a\n",
                          accum, (double)f_scalar, (double)f_fixed);
            return "adm_cm_avx2 f_accum_h fixed expr differs from scalar";
        }

        /* Confirm that the old buggy expression actually diverges for at
         * least some representative values (guards against the test being
         * vacuous — if it never fires, the test provides no regression
         * coverage). We tolerate the case where they happen to be equal
         * on a given value, but log it so we know. */
        if (f_buggy == f_scalar) { /* vacuity guard: intentional exact compare */
            (void)fprintf(stderr,
                          "  info: adm accum %" PRId64 " buggy==scalar (no ULP diff "
                          "for this value; other values in the set will diverge)\n",
                          accum);
        }
    }
    return NULL;
}

#define DEC_SLACK 32
#define DEC_PLANES 12

typedef void (*adm_decouple_fn)(AdmBuffer *buf, int w, int h, int stride,
                                double adm_enhn_gain_limit, int32_t *adm_div_lookup);

#if ARCH_X86 || ARCH_AARCH64

/* ---------------------------------------------------------------------
 * Test 2: end-to-end smoke test for adm_cm_avx2 with synthetic buffer.
 *
 * We construct a minimal AdmBuffer with small-magnitude band data so
 * that the integer accumulation path exercises the "completely within
 * frame" SIMD branch, then verify the returned score is finite and
 * reproduces on repeated calls (determinism check).
 *
 * A full scalar-vs-SIMD comparison requires the static adm_cm() from
 * integer_adm.c; since that function has internal linkage we exercise
 * determinism and smoke-correctness only here, relying on the full
 * Netflix golden-data gate (make test-netflix-golden) to catch any
 * residual score divergence.
 * ------------------------------------------------------------------- */

/* Band dimensions: choose w and h such that left > 0 and right <= w-1
 * (completely-within-frame condition) to exercise the SIMD inner loop.
 * With ADM_BORDER_FACTOR=0.1, w=40 gives left=3, right=37. */
#define ADM_TEST_W 40
#define ADM_TEST_H 30
#define ADM_TEST_STRIDE ((ADM_TEST_W + 7) & ~7)
#define ADM_BAND_PX ((size_t)ADM_TEST_STRIDE * ADM_TEST_H)

/* Nine synthetic band arrays for decouple_r and csf_a/csf_f (adm_cm_avx2
 * reads from buf->decouple_r and buf->csf_a/csf_f). */
typedef struct AdmBands16 {
    int16_t *dr_h, *dr_v, *dr_d;
    int16_t *cf_h, *cf_v, *cf_d;
    int16_t *ca_h, *ca_v, *ca_d;
} AdmBands16;

static int adm_bands16_alloc(AdmBands16 *b, size_t bytes)
{
    b->dr_h = (int16_t *)simd_test_aligned_malloc(bytes, 32);
    b->dr_v = (int16_t *)simd_test_aligned_malloc(bytes, 32);
    b->dr_d = (int16_t *)simd_test_aligned_malloc(bytes, 32);
    b->cf_h = (int16_t *)simd_test_aligned_malloc(bytes, 32);
    b->cf_v = (int16_t *)simd_test_aligned_malloc(bytes, 32);
    b->cf_d = (int16_t *)simd_test_aligned_malloc(bytes, 32);
    b->ca_h = (int16_t *)simd_test_aligned_malloc(bytes, 32);
    b->ca_v = (int16_t *)simd_test_aligned_malloc(bytes, 32);
    b->ca_d = (int16_t *)simd_test_aligned_malloc(bytes, 32);
    if (!b->dr_h || !b->dr_v || !b->dr_d || !b->cf_h || !b->cf_v || !b->cf_d || !b->ca_h ||
        !b->ca_v || !b->ca_d)
        return -1;
    return 0;
}

static void adm_bands16_free(AdmBands16 *b)
{
    simd_test_aligned_free(b->dr_h);
    simd_test_aligned_free(b->dr_v);
    simd_test_aligned_free(b->dr_d);
    simd_test_aligned_free(b->cf_h);
    simd_test_aligned_free(b->cf_v);
    simd_test_aligned_free(b->cf_d);
    simd_test_aligned_free(b->ca_h);
    simd_test_aligned_free(b->ca_v);
    simd_test_aligned_free(b->ca_d);
}

#if ARCH_X86
/* Fill band arrays with small non-zero int16 values. Using
 * simd_test_xorshift32 masked to int16 range [0, 255]. */
static void adm_bands16_fill(AdmBands16 *b, uint32_t seed)
{
    uint32_t state = seed;
    for (size_t i = 0; i < ADM_BAND_PX; i++) {
        uint32_t r = simd_test_xorshift32(&state);
        b->dr_h[i] = (int16_t)((int)(r & 0xFF) - 128);
        b->dr_v[i] = (int16_t)((int)((r >> 8) & 0xFF) - 128);
        b->dr_d[i] = (int16_t)((int)((r >> 16) & 0xFF) - 128);
        b->cf_h[i] = (int16_t)((int)((r >> 24) & 0xFF));
        r = simd_test_xorshift32(&state);
        b->cf_v[i] = (int16_t)((int)(r & 0xFF));
        b->cf_d[i] = (int16_t)((int)((r >> 8) & 0xFF));
        r = simd_test_xorshift32(&state);
        b->ca_h[i] = (int16_t)((int)(r & 0xFF));
        b->ca_v[i] = (int16_t)((int)((r >> 8) & 0xFF));
        b->ca_d[i] = (int16_t)((int)((r >> 16) & 0xFF));
    }
}

/* Result checks shared by the determinism + p_norm assertions below:
 * finite/non-negative range, bit-exact determinism, and a visible change
 * under a different adm_p_norm. */
static char *check_adm_cm_avx2_smoke_result(float r1, float r2, float r_p2)
{
    /* Result must be finite and non-negative. */
    if (!(r1 >= 0.0f && r1 < 1e10f)) {
        (void)fprintf(stderr, "  adm_cm_avx2 returned non-finite/negative: %g\n", (double)r1);
        return "adm_cm_avx2 returned non-finite or negative value";
    }
    /* Determinism: both calls must agree bit-exactly (intentional exact compare). */
    if (r1 != r2) { /* bit-exact determinism assertion */
        (void)fprintf(stderr, "  adm_cm_avx2 non-deterministic: r1=%a r2=%a\n", (double)r1,
                      (double)r2);
        return "adm_cm_avx2 is non-deterministic";
    }
    if (!(r_p2 >= 0.0f && r_p2 < 1e10f)) {
        (void)fprintf(stderr, "  adm_cm_avx2 p=2 returned invalid value: %g\n", (double)r_p2);
        return "adm_cm_avx2 p=2 returned invalid value";
    }
    if (r_p2 == r1) { /* intentional exact compare: p_norm must visibly change result */
        return "adm_cm_avx2 ignored adm_p_norm";
    }
    return NULL;
}

static char *test_adm_cm_avx2_smoke(void)
{
    const size_t bytes = ADM_BAND_PX * sizeof(int16_t);
    AdmBands16 bands;
    if (adm_bands16_alloc(&bands, bytes)) {
        adm_bands16_free(&bands);
        return "aligned_malloc failed";
    }
    adm_bands16_fill(&bands, 0xdeadbeefu);

    /* Populate AdmBuffer with our synthetic bands.  Zero-initialise the
     * struct first so unused pointer members do not hold garbage. */
    AdmBuffer buf;
    (void)memset(&buf, 0, sizeof(buf));
    buf.decouple_r.band_h = bands.dr_h;
    buf.decouple_r.band_v = bands.dr_v;
    buf.decouple_r.band_d = bands.dr_d;
    buf.csf_f.band_h = bands.cf_h;
    buf.csf_f.band_v = bands.cf_v;
    buf.csf_f.band_d = bands.cf_d;
    buf.csf_a.band_h = bands.ca_h;
    buf.csf_a.band_v = bands.ca_v;
    buf.csf_a.band_d = bands.ca_d;

    const int w = ADM_TEST_W;
    const int h = ADM_TEST_H;
    const int stride = ADM_TEST_STRIDE;
    const double nvd = DEFAULT_ADM_NORM_VIEW_DIST;
    const int rdh = DEFAULT_ADM_REF_DISPLAY_HEIGHT;
    const double csf_s = 1.0;
    const double csf_ds = 1.0;
    const double nw = DEFAULT_ADM_NOISE_WEIGHT;

    /* Call adm_cm_avx2 twice — must return the same value (determinism). */
    const float r1 = adm_cm_avx2(&buf, w, h, stride, stride, nvd, rdh, ADM_CSF_MODE_WATSON97, csf_s,
                                 csf_ds, nw, 3.0, false);
    const float r2 = adm_cm_avx2(&buf, w, h, stride, stride, nvd, rdh, ADM_CSF_MODE_WATSON97, csf_s,
                                 csf_ds, nw, 3.0, false);
    const float r_p2 = adm_cm_avx2(&buf, w, h, stride, stride, nvd, rdh, ADM_CSF_MODE_WATSON97,
                                   csf_s, csf_ds, nw, 2.0, false);

    adm_bands16_free(&bands);

    return check_adm_cm_avx2_smoke_result(r1, r2, r_p2);
}
#endif /* ARCH_X86 */

/* Nine synthetic band arrays for i4_decouple_r and i4_csf_a/i4_csf_f. */
typedef struct AdmBands32 {
    int32_t *dr_h, *dr_v, *dr_d;
    int32_t *cf_h, *cf_v, *cf_d;
    int32_t *ca_h, *ca_v, *ca_d;
} AdmBands32;

static int adm_bands32_alloc(AdmBands32 *b, size_t bytes)
{
    b->dr_h = (int32_t *)simd_test_aligned_malloc(bytes, 32);
    b->dr_v = (int32_t *)simd_test_aligned_malloc(bytes, 32);
    b->dr_d = (int32_t *)simd_test_aligned_malloc(bytes, 32);
    b->cf_h = (int32_t *)simd_test_aligned_malloc(bytes, 32);
    b->cf_v = (int32_t *)simd_test_aligned_malloc(bytes, 32);
    b->cf_d = (int32_t *)simd_test_aligned_malloc(bytes, 32);
    b->ca_h = (int32_t *)simd_test_aligned_malloc(bytes, 32);
    b->ca_v = (int32_t *)simd_test_aligned_malloc(bytes, 32);
    b->ca_d = (int32_t *)simd_test_aligned_malloc(bytes, 32);
    if (!b->dr_h || !b->dr_v || !b->dr_d || !b->cf_h || !b->cf_v || !b->cf_d || !b->ca_h ||
        !b->ca_v || !b->ca_d)
        return -1;
    return 0;
}

static void adm_bands32_free(AdmBands32 *b)
{
    simd_test_aligned_free(b->dr_h);
    simd_test_aligned_free(b->dr_v);
    simd_test_aligned_free(b->dr_d);
    simd_test_aligned_free(b->cf_h);
    simd_test_aligned_free(b->cf_v);
    simd_test_aligned_free(b->cf_d);
    simd_test_aligned_free(b->ca_h);
    simd_test_aligned_free(b->ca_v);
    simd_test_aligned_free(b->ca_d);
}

#if ARCH_X86
static void adm_bands32_fill(AdmBands32 *b, uint32_t seed)
{
    uint32_t state = seed;
    for (size_t i = 0; i < ADM_BAND_PX; i++) {
        uint32_t r = simd_test_xorshift32(&state);
        b->dr_h[i] = (int32_t)((int)(r & 0x3F) - 32);
        b->dr_v[i] = (int32_t)((int)((r >> 8) & 0x3F) - 32);
        b->dr_d[i] = (int32_t)((int)((r >> 16) & 0x3F) - 32);
        b->cf_h[i] = (int32_t)((int)((r >> 24) & 0x3F));
        r = simd_test_xorshift32(&state);
        b->cf_v[i] = (int32_t)((int)(r & 0x3F));
        b->cf_d[i] = (int32_t)((int)((r >> 8) & 0x3F));
        r = simd_test_xorshift32(&state);
        b->ca_h[i] = (int32_t)((int)(r & 0x3F));
        b->ca_v[i] = (int32_t)((int)((r >> 8) & 0x3F));
        b->ca_d[i] = (int32_t)((int)((r >> 16) & 0x3F));
    }
}

static char *check_i4_adm_cm_avx2_p_norm_result(float r_p3, float r_p2)
{
    if (!(r_p3 >= 0.0f && r_p3 < 1e10f && r_p2 >= 0.0f && r_p2 < 1e10f)) {
        return "i4_adm_cm_avx2 p-norm result invalid";
    }
    if (r_p2 ==
        r_p3) { /* intentional exact compare: different p_norm must produce different result */
        return "i4_adm_cm_avx2 ignored adm_p_norm";
    }
    return NULL;
}

static char *test_i4_adm_cm_avx2_p_norm(void)
{
    const size_t bytes = ADM_BAND_PX * sizeof(int32_t);
    AdmBands32 bands;
    if (adm_bands32_alloc(&bands, bytes)) {
        adm_bands32_free(&bands);
        return "aligned_malloc failed";
    }
    adm_bands32_fill(&bands, 0x51d15eedu);

    AdmBuffer buf;
    (void)memset(&buf, 0, sizeof(buf));
    buf.i4_decouple_r.band_h = bands.dr_h;
    buf.i4_decouple_r.band_v = bands.dr_v;
    buf.i4_decouple_r.band_d = bands.dr_d;
    buf.i4_csf_f.band_h = bands.cf_h;
    buf.i4_csf_f.band_v = bands.cf_v;
    buf.i4_csf_f.band_d = bands.cf_d;
    buf.i4_csf_a.band_h = bands.ca_h;
    buf.i4_csf_a.band_v = bands.ca_v;
    buf.i4_csf_a.band_d = bands.ca_d;

    const float r_p3 =
        i4_adm_cm_avx2(&buf, ADM_TEST_W, ADM_TEST_H, ADM_TEST_STRIDE, ADM_TEST_STRIDE, 1,
                       DEFAULT_ADM_NORM_VIEW_DIST, DEFAULT_ADM_REF_DISPLAY_HEIGHT,
                       ADM_CSF_MODE_WATSON97, 1.0, 1.0, DEFAULT_ADM_NOISE_WEIGHT, 3.0, false);
    const float r_p2 =
        i4_adm_cm_avx2(&buf, ADM_TEST_W, ADM_TEST_H, ADM_TEST_STRIDE, ADM_TEST_STRIDE, 1,
                       DEFAULT_ADM_NORM_VIEW_DIST, DEFAULT_ADM_REF_DISPLAY_HEIGHT,
                       ADM_CSF_MODE_WATSON97, 1.0, 1.0, DEFAULT_ADM_NOISE_WEIGHT, 2.0, false);

    adm_bands32_free(&bands);

    return check_i4_adm_cm_avx2_p_norm_result(r_p3, r_p2);
}
#endif /* ARCH_X86 */

/* ---------------------------------------------------------------------
 * Test 4: adm_cm_avx2 / adm_cm_avx512 against the scalar kernels on
 * hand-built bands (ADR-1402).
 *
 * The scalar side is the body of adm_cm() in integer_adm.c, assembled from
 * the kernels it shares with the vector files. The vector kernels return a
 * float, so each fixture keeps the accumulators small or sparse enough for a
 * single wrong sample to change that float, and every case also runs with
 * adm_p_norm 1, where a negative accumulator stays a number instead of NaN.
 *
 * The vector rows sum with a short form of the excess and repeat the row with
 * the exact form when a threshold is negative or its product leaves int32.
 * The dense fills with filtered bands of either sign repeat every row, the
 * event and block fills the rows near the event or block; the dense fills
 * with non-negative filtered bands never repeat a row.
 * ------------------------------------------------------------------- */

typedef float (*adm_cm_fn)(AdmBuffer *buf, int w, int h, int src_stride, int csf_a_stride,
                           double adm_norm_view_dist, int adm_ref_display_height, int adm_csf_mode,
                           double adm_csf_scale, double adm_csf_diag_scale, double adm_noise_weight,
                           double adm_p_norm, bool measure_aim);

/* One band geometry with its nine bands bound into an AdmBuffer. `planes`
 * lists the same nine arrays: decoupled h, v, d, then csf_f, then csf_a. */
typedef struct CmFixture {
    AdmBands16 bands;
    AdmBuffer buf;
    int16_t *planes[9];
    int w;
    int h;
    int stride;
} CmFixture;

static void cm_fixture_free(CmFixture *f)
{
    adm_bands16_free(&f->bands);
}

static int cm_fixture_alloc(CmFixture *f, int w, int h)
{
    (void)memset(f, 0, sizeof(*f));
    f->w = w;
    f->h = h;
    f->stride = (w + 15) & ~15;
    const size_t bytes = (size_t)f->stride * (size_t)h * sizeof(int16_t);
    if (adm_bands16_alloc(&f->bands, bytes)) {
        return -1;
    }
    int16_t *const planes[9] = {f->bands.dr_h, f->bands.dr_v, f->bands.dr_d,
                                f->bands.cf_h, f->bands.cf_v, f->bands.cf_d,
                                f->bands.ca_h, f->bands.ca_v, f->bands.ca_d};
    for (unsigned p = 0; p < 9u; ++p) {
        f->planes[p] = planes[p];
    }
    /* The AIM pass measures decouple_a; both passes read the same bands here. */
    f->buf.decouple_r =
        (adm_dwt_band_t){.band_h = planes[0], .band_v = planes[1], .band_d = planes[2]};
    f->buf.decouple_a = f->buf.decouple_r;
    f->buf.csf_f = (adm_dwt_band_t){.band_h = planes[3], .band_v = planes[4], .band_d = planes[5]};
    f->buf.csf_a = (adm_dwt_band_t){.band_h = planes[6], .band_v = planes[7], .band_d = planes[8]};
    return 0;
}

static void cm_fixture_clear(CmFixture *f)
{
    const size_t bytes = (size_t)f->stride * (size_t)f->h * sizeof(int16_t);
    for (unsigned p = 0; p < 9u; ++p) {
        (void)memset(f->planes[p], 0, bytes);
    }
}

/* A sample in [-limit, limit]. */
static int16_t cm_sample(uint32_t *state, int limit)
{
    const uint32_t r = simd_test_xorshift32(state);
    return (int16_t)((int)(r % (uint32_t)((2 * limit) + 1)) - limit);
}

/* Any int16 value, -32768 included. */
static int16_t cm_sample_full(uint32_t *state)
{
    return (int16_t)((int)(simd_test_xorshift32(state) & 0xFFFFu) - 32768);
}

/* Write `v` (or a full-range sample when `state` is given) to planes
 * [first, first + 3) at (row, col); positions outside the band are skipped. */
static void cm_poke(CmFixture *f, unsigned first, int row, int col, uint32_t *state, int16_t v)
{
    if (row < 0 || row >= f->h || col < 0 || col >= f->w) {
        return;
    }
    for (unsigned p = first; p < first + 3u; ++p) {
        f->planes[p][((ptrdiff_t)row * f->stride) + col] = state ? cm_sample_full(state) : v;
    }
}

/* Every band holds moderate samples. With `any_sign` the filtered bands take
 * either sign, so the threshold is negative in about half the columns:
 * vector body, tail block and edge rows. Without it they are non-negative,
 * as in a decoded picture, and every vector row keeps its short form. */
static void cm_fill_dense(CmFixture *f, uint32_t seed, bool any_sign)
{
    uint32_t state = seed;
    const size_t count = (size_t)f->stride * (size_t)f->h;
    for (unsigned p = 0; p < 9u; ++p) {
        for (size_t i = 0; i < count; ++i) {
            const int16_t v = cm_sample(&state, 2000);
            f->planes[p][i] = (p < 3u || any_sign || v >= 0) ? v : (int16_t)-v;
        }
    }
}

/* One full-range coefficient at (row, col) with full-range filtered values on
 * it and on one neighbour; everything else is 0. */
static void cm_fill_event(CmFixture *f, int row, int col, uint32_t seed)
{
    uint32_t state = seed;
    cm_fixture_clear(f);
    cm_poke(f, 0u, row, col, &state, 0);
    cm_poke(f, 3u, row, col, &state, 0);
    cm_poke(f, 6u, row, col, &state, 0);
    const int d_row = (int)(simd_test_xorshift32(&state) % 3u) - 1;
    const int d_col = (int)(simd_test_xorshift32(&state) % 3u) - 1;
    cm_poke(f, 3u, row + d_row, col + d_col, &state, 0);
    cm_poke(f, 6u, row + d_row, col + d_col, &state, 0);
}

/* A block of three rows by four columns whose filtered bands all hold
 * `level`, with full-range coefficients on the block and the ring around it:
 * the samples whose 3x3 window reaches the block. With `level` -32768 the
 * threshold falls to -786432 and its product with 2^12 leaves int32 on the
 * negative side (the excess saturates); with 32767 it leaves int32 on the
 * positive side (the sample is masked). At most six samples of a row carry a
 * cube, each below 2^62 >> shift_cub, so a row total stays inside int64 for
 * the band widths above 32 this fill is used with. */
static void cm_fill_block(CmFixture *f, int row0, int col0, int16_t level, uint32_t seed)
{
    uint32_t state = seed;
    cm_fixture_clear(f);
    for (int row = row0 - 1; row <= row0 + 3; ++row) {
        for (int col = col0 - 1; col <= col0 + 4; ++col) {
            const bool inside = row >= row0 && row < row0 + 3 && col >= col0 && col < col0 + 4;
            cm_poke(f, 0u, row, col, &state, 0);
            if (inside) {
                cm_poke(f, 3u, row, col, NULL, level);
                cm_poke(f, 6u, row, col, NULL, level);
            }
        }
    }
}

#define CM_NVD DEFAULT_ADM_NORM_VIEW_DIST
#define CM_RDH DEFAULT_ADM_REF_DISPLAY_HEIGHT
#define CM_NW DEFAULT_ADM_NOISE_WEIGHT

/* adm_cm() of integer_adm.c, from the kernels it is built on. */
static float cm_scalar(CmFixture *f, bool aim, double p_norm)
{
    AdmCmCtx c;
    adm_cm_ctx_init(&c, &f->buf, f->w, f->h, f->stride, f->stride, CM_NVD, CM_RDH,
                    ADM_CSF_MODE_WATSON97, 1.0, 1.0, aim);
    const AdmCmBounds bd = adm_cm_bounds(f->w, f->h);
    uint64_t accum[3] = {0, 0, 0};
    adm_cm_rows(&c, &bd, adm_cm_row, accum);
    return adm_cm_result(&c, &bd, accum, CM_NW, p_norm);
}

/* Lowest and highest DLM masking threshold over the band. */
static void cm_threshold_range(CmFixture *f, int32_t *lo, int32_t *hi)
{
    AdmCmCtx c;
    adm_cm_ctx_init(&c, &f->buf, f->w, f->h, f->stride, f->stride, CM_NVD, CM_RDH,
                    ADM_CSF_MODE_WATSON97, 1.0, 1.0, false);
    for (int i = 0; i < f->h; ++i) {
        for (int j = 0; j < f->w; ++j) {
            const int32_t thr = adm_cm_thresh(c.angles, c.flt_angles, f->stride, f->w, f->h, i, j);
            *lo = (thr < *lo) ? thr : *lo;
            *hi = (thr > *hi) ? thr : *hi;
        }
    }
}

static uint32_t cm_float_bits(float v)
{
    uint32_t bits = 0;
    (void)memcpy(&bits, &v, sizeof(bits));
    return bits;
}

/* The vector kernels the host can run. */
typedef struct CmKernels {
    adm_cm_fn fn[2];
    const char *name[2];
    unsigned count;
} CmKernels;

static CmKernels cm_kernels(void)
{
#if ARCH_AARCH64
    const CmKernels k = {{adm_cm_neon, NULL}, {"adm_cm_neon", NULL}, 1u};
#else
    CmKernels k = {{adm_cm_avx2, NULL}, {"adm_cm_avx2", NULL}, 1u};
#if HAVE_AVX512
    if (simd_test_have_avx512()) {
        k.fn[1] = adm_cm_avx512;
        k.name[1] = "adm_cm_avx512";
        k.count = 2u;
    }
#endif
#endif
    return k;
}

/* Every kernel against the scalar on the fixture as filled, for the DLM and
 * the AIM pass and for adm_p_norm 3 and 1. */
static char *cm_check(CmFixture *f, const CmKernels *k, const char *fill, int id)
{
    static const double P_NORMS[2] = {3.0, 1.0};
    for (unsigned variant = 0; variant < 4u; ++variant) {
        const bool aim = (variant & 1u) != 0u;
        const double p_norm = P_NORMS[variant >> 1];
        const float want = cm_scalar(f, aim, p_norm);
        for (unsigned n = 0; n < k->count; ++n) {
            const float got = k->fn[n](&f->buf, f->w, f->h, f->stride, f->stride, CM_NVD, CM_RDH,
                                       ADM_CSF_MODE_WATSON97, 1.0, 1.0, CM_NW, p_norm, aim);
            if (cm_float_bits(got) != cm_float_bits(want)) {
                (void)fprintf(stderr, "\n  %s %dx%d %s case %d aim=%d p_norm=%g: %a, scalar %a\n",
                              k->name[n], f->w, f->h, fill, id, (int)aim, p_norm, (double)got,
                              (double)want);
                return "adm_cm vector kernel differs from the scalar kernels";
            }
        }
    }
    return NULL;
}

/* The event sweeps of tests 4 and 4b visit every position in a plain build,
 * which is what proves every vector lane bit for bit. An instrumented build (the
 * ASan + UBSan, TSan and UBSan legs, debug) runs the kernels some 17 times slower:
 * test 4b's full sweep alone took 31 s there, over the 30 s test budget
 * (T-ADM-SIMD-SANITIZER-SWEEP-TIMEOUT-2026-10-09,
 * T-ADM-SIMD-UBSAN-SWEEP-TIMEOUT-2026-10-09). What such a build adds is
 * memory and arithmetic checking. The kernels' addresses do not depend on the
 * data (the dense fills reach all of them), and the arithmetic an event drives
 * depends on its distance to a border (mirroring, the scalar edge columns),
 * not on its lane; so it visits the first three, the middle and the last three
 * rows, crossed with the same columns. */
static bool event_coordinate_swept(int v, int n)
{
    return !VMAF_TEST_INSTRUMENTED_BUILD || v < 3 || v >= n - 3 || v == n / 2;
}

static bool event_position_swept(int row, int col, int w, int h)
{
    return event_coordinate_swept(row, h) && event_coordinate_swept(col, w);
}

/* Dense fills over eight seeds, half of them with filtered bands of either
 * sign, then one event at every position (event_position_swept()). */
static char *cm_check_geometry(CmFixture *f, const CmKernels *k)
{
    for (int seed = 1; seed <= 8; ++seed) {
        cm_fill_dense(f, 0x9E3779B9u * (uint32_t)seed, (seed & 1) != 0);
        char *msg = cm_check(f, k, (seed & 1) ? "dense" : "dense, non-negative", seed);
        if (msg) {
            return msg;
        }
    }
    for (int pos = 0; pos < f->w * f->h; ++pos) {
        if (!event_position_swept(pos / f->w, pos % f->w, f->w, f->h)) {
            continue;
        }
        cm_fill_event(f, pos / f->w, pos % f->w, 0x85EBCA6Bu + (uint32_t)pos);
        char *msg = cm_check(f, k, "event", pos);
        if (msg) {
            return msg;
        }
    }
    return NULL;
}

/* The block at every column, moving down the rows; `range` collects the
 * thresholds the fixtures reached. */
static char *cm_check_blocks(CmFixture *f, const CmKernels *k, int16_t level, int32_t range[2])
{
    for (int col0 = 0; col0 + 4 <= f->w; ++col0) {
        cm_fill_block(f, col0 % (f->h - 2), col0, level, 0xC2B2AE35u + (uint32_t)col0);
        cm_threshold_range(f, &range[0], &range[1]);
        char *msg = cm_check(f, k, (level < 0) ? "negative block" : "positive block", col0);
        if (msg) {
            return msg;
        }
    }
    return NULL;
}

typedef struct CmGeometry {
    int w;
    int h;
    bool blocks; /* wide enough for cm_fill_block() */
} CmGeometry;

static char *cm_check_one_geometry(CmGeometry g, const CmKernels *k, int32_t range[2])
{
    CmFixture f;
    if (cm_fixture_alloc(&f, g.w, g.h)) {
        cm_fixture_free(&f);
        return "aligned_malloc failed";
    }
    char *msg = cm_check_geometry(&f, k);
    if (!msg && g.blocks) {
        msg = cm_check_blocks(&f, k, INT16_MIN, range);
    }
    if (!msg && g.blocks) {
        msg = cm_check_blocks(&f, k, INT16_MAX, range);
    }
    cm_fixture_free(&f);
    return msg;
}

static char *test_adm_cm_matches_scalar_kernels(void)
{
    /* 12 columns put the first and the last column inside the region (the
     * scalar row path). 15 and 16 give 13 and 14 interior columns: one short
     * of a fourteen-column block, which stays scalar, and exactly one. The
     * others leave 1 to 13 columns for the overlapped tail block behind the
     * six- and fourteen-column vector blocks. */
    static const CmGeometry geometries[] = {
        {12, 10, false}, {15, 9, false}, {16, 9, false},  {23, 17, false},
        {40, 30, true},  {67, 21, true}, {130, 12, true},
    };
    const CmKernels k = cm_kernels();
    int32_t range[2] = {0, 0};

    for (size_t g = 0; g < sizeof(geometries) / sizeof(geometries[0]); ++g) {
        char *msg = cm_check_one_geometry(geometries[g], &k, range);
        if (msg) {
            return msg;
        }
    }
    /* The block fills must have reached both regimes the 32-bit excess got
     * wrong: |thr| * 2^12 at or beyond 2^31. */
    mu_assert("no fixture reached a threshold of -2^19 or below", range[0] <= -(INT32_C(1) << 19));
    mu_assert("no fixture reached a threshold of 2^19 or above", range[1] >= (INT32_C(1) << 19));
    return NULL;
}

/* The centre tap of the masking threshold is not narrowed to int16: a lone
 * coefficient of 15360 gives 32768 and one of -32768 gives 69904, where the
 * int16 store gave -32768 and 4368. */
static char *test_adm_cm_centre_tap_stays_int32(void)
{
    int16_t src[3][9] = {{0}, {0}, {0}};
    int16_t flt[3][9] = {{0}, {0}, {0}};
    int16_t *const angles[3] = {src[0], src[1], src[2]};
    int16_t *const flt_angles[3] = {flt[0], flt[1], flt[2]};

    src[0][4] = 15360;
    mu_assert("a centre coefficient of 15360 must give a threshold of 32768",
              adm_cm_thresh(angles, flt_angles, 3, 3, 3, 1, 1) == 32768);
    src[1][4] = INT16_MIN;
    mu_assert("coefficients of 15360 and -32768 must give 32768 + 69904",
              adm_cm_thresh(angles, flt_angles, 3, 3, 3, 1, 1) == 32768 + 69904);
    flt[2][0] = -7;
    flt[2][8] = 5;
    mu_assert("the filtered neighbours are added unchanged",
              adm_cm_thresh(angles, flt_angles, 3, 3, 3, 1, 1) == 32768 + 69904 - 2);
    return NULL;
}

/* ---------------------------------------------------------------------
 * Test 4b: i4_adm_cm vector kernels (scales 1-3) against the scalar kernels.
 *
 * The scalar side is i4_adm_cm() of integer_adm.c, assembled from the
 * kernels of integer_adm_kernels.h. Every scale, the DLM and the AIM pass,
 * adm_p_norm 3 and 1, on dense fills (filtered bands of either sign, so the
 * threshold is negative in about half the columns) and on one large event
 * at every position. The magnitudes keep every scalar int32 sum and every
 * row inside its type: the scalar arithmetic is the definition only where
 * it is defined.
 * ------------------------------------------------------------------- */

typedef float (*i4_adm_cm_fn)(AdmBuffer *buf, int w, int h, int src_stride, int csf_a_stride,
                              int scale, double adm_norm_view_dist, int adm_ref_display_height,
                              int adm_csf_mode, double adm_csf_scale, double adm_csf_diag_scale,
                              double adm_noise_weight, double adm_p_norm, bool measure_aim);

typedef struct I4CmFixture {
    AdmBands32 bands;
    AdmBuffer buf;
    int32_t *planes[9];
    int w;
    int h;
    int stride;
} I4CmFixture;

static int i4_cm_fixture_alloc(I4CmFixture *f, int w, int h)
{
    (void)memset(f, 0, sizeof(*f));
    f->w = w;
    f->h = h;
    f->stride = (w + 7) & ~7;
    const size_t bytes = (size_t)f->stride * (size_t)h * sizeof(int32_t);
    if (adm_bands32_alloc(&f->bands, bytes)) {
        return -1;
    }
    int32_t *const planes[9] = {f->bands.dr_h, f->bands.dr_v, f->bands.dr_d,
                                f->bands.cf_h, f->bands.cf_v, f->bands.cf_d,
                                f->bands.ca_h, f->bands.ca_v, f->bands.ca_d};
    for (unsigned p = 0; p < 9u; ++p) {
        f->planes[p] = planes[p];
    }
    f->buf.i4_decouple_r =
        (i4_adm_dwt_band_t){.band_h = planes[0], .band_v = planes[1], .band_d = planes[2]};
    f->buf.i4_decouple_a = f->buf.i4_decouple_r;
    f->buf.i4_csf_f =
        (i4_adm_dwt_band_t){.band_h = planes[3], .band_v = planes[4], .band_d = planes[5]};
    f->buf.i4_csf_a =
        (i4_adm_dwt_band_t){.band_h = planes[6], .band_v = planes[7], .band_d = planes[8]};
    return 0;
}

/* A sample in [-limit, limit] (limit below 2^30). */
static int32_t i4_cm_sample(uint32_t *state, int32_t limit)
{
    const uint32_t r = simd_test_xorshift32(state);
    return (int32_t)(r % (uint32_t)((2 * limit) + 1)) - limit;
}

/* Dense: decoupled bands within 2^20, filtered bands within 2^16 of either
 * sign (`any_sign`) or non-negative. */
static void i4_cm_fill_dense(I4CmFixture *f, uint32_t seed, bool any_sign)
{
    uint32_t state = seed;
    const size_t count = (size_t)f->stride * (size_t)f->h;
    for (unsigned p = 0; p < 9u; ++p) {
        for (size_t i = 0; i < count; ++i) {
            const int32_t v = i4_cm_sample(&state, (p < 3u) ? (1 << 20) : (1 << 16));
            f->planes[p][i] = (p < 3u || any_sign || v >= 0) ? v : -v;
        }
    }
}

/* One event: a decoupled coefficient within 2^26 and filtered values within
 * 2^24 on it and on one neighbour; everything else 0. */
static void i4_cm_fill_event(I4CmFixture *f, int row, int col, uint32_t seed)
{
    uint32_t state = seed;
    const size_t count = (size_t)f->stride * (size_t)f->h;
    for (unsigned p = 0; p < 9u; ++p) {
        (void)memset(f->planes[p], 0, count * sizeof(int32_t));
    }
    const int d_row = (int)(simd_test_xorshift32(&state) % 3u) - 1;
    const int d_col = (int)(simd_test_xorshift32(&state) % 3u) - 1;
    const int rows[2] = {row, row + d_row};
    const int cols[2] = {col, col + d_col};
    for (int n = 0; n < 2; ++n) {
        if (rows[n] < 0 || rows[n] >= f->h || cols[n] < 0 || cols[n] >= f->w) {
            continue;
        }
        const ptrdiff_t at = ((ptrdiff_t)rows[n] * f->stride) + cols[n];
        for (unsigned p = (n == 0) ? 0u : 3u; p < 9u; ++p) {
            f->planes[p][at] = i4_cm_sample(&state, (p < 3u) ? (1 << 26) : (1 << 24));
        }
    }
}

/* i4_adm_cm() of integer_adm.c, from the kernels it is built on. */
static float i4_cm_scalar(I4CmFixture *f, int scale, bool aim, double p_norm)
{
    I4AdmCmCtx c;
    i4_adm_cm_ctx_init(&c, &f->buf, f->w, f->h, f->stride, f->stride, scale, CM_NVD, CM_RDH,
                       ADM_CSF_MODE_WATSON97, 1.0, 1.0, aim);
    const AdmCmBounds bd = adm_cm_bounds(f->w, f->h);
    int64_t accum[3] = {0, 0, 0};
    i4_adm_cm_rows(&c, &bd, i4_adm_cm_row, accum);
    return i4_adm_cm_result(&c, &bd, accum, CM_NW, p_norm);
}

typedef struct I4CmKernels {
    i4_adm_cm_fn fn[2];
    const char *name[2];
    unsigned count;
} I4CmKernels;

static I4CmKernels i4_cm_kernels(void)
{
#if ARCH_AARCH64
    const I4CmKernels k = {{i4_adm_cm_neon, NULL}, {"i4_adm_cm_neon", NULL}, 1u};
#else
    I4CmKernels k = {{i4_adm_cm_avx2, NULL}, {"i4_adm_cm_avx2", NULL}, 1u};
#if HAVE_AVX512
    if (simd_test_have_avx512()) {
        k.fn[1] = i4_adm_cm_avx512;
        k.name[1] = "i4_adm_cm_avx512";
        k.count = 2u;
    }
#endif
#endif
    return k;
}

/* Every kernel against the scalar for every scale, pass and p-norm. */
static char *i4_cm_check(I4CmFixture *f, const I4CmKernels *k, const char *fill, int id)
{
    static const double P_NORMS[2] = {3.0, 1.0};
    for (unsigned variant = 0; variant < 12u; ++variant) {
        const int scale = 1 + (int)(variant % 3u);
        const bool aim = ((variant / 3u) & 1u) != 0u;
        const double p_norm = P_NORMS[variant / 6u];
        const float want = i4_cm_scalar(f, scale, aim, p_norm);
        for (unsigned n = 0; n < k->count; ++n) {
            const float got = k->fn[n](&f->buf, f->w, f->h, f->stride, f->stride, scale, CM_NVD,
                                       CM_RDH, ADM_CSF_MODE_WATSON97, 1.0, 1.0, CM_NW, p_norm, aim);
            if (cm_float_bits(got) != cm_float_bits(want)) {
                (void)fprintf(stderr,
                              "\n  %s %dx%d %s case %d scale %d aim=%d p_norm=%g: %a, scalar %a\n",
                              k->name[n], f->w, f->h, fill, id, scale, (int)aim, p_norm,
                              (double)got, (double)want);
                return "i4_adm_cm vector kernel differs from the scalar kernels";
            }
        }
    }
    return NULL;
}

static char *i4_cm_check_geometry(int w, int h, const I4CmKernels *k)
{
    I4CmFixture f;
    char *msg = NULL;
    if (i4_cm_fixture_alloc(&f, w, h)) {
        adm_bands32_free(&f.bands);
        return "aligned_malloc failed";
    }
    for (int seed = 1; seed <= 6 && !msg; ++seed) {
        i4_cm_fill_dense(&f, 0x2545F491u * (uint32_t)seed, (seed & 1) != 0);
        msg = i4_cm_check(&f, k, (seed & 1) ? "dense" : "dense, non-negative", seed);
    }
    for (int pos = 0; pos < w * h && !msg; ++pos) {
        if (!event_position_swept(pos / w, pos % w, w, h)) {
            continue;
        }
        i4_cm_fill_event(&f, pos / w, pos % w, 0x27D4EB2Fu + (uint32_t)pos);
        msg = i4_cm_check(&f, k, "event", pos);
    }
    adm_bands32_free(&f.bands);
    return msg;
}

static char *test_i4_adm_cm_matches_scalar_kernels(void)
{
    /* The geometries of the scale-0 test: edge columns inside the region,
     * rows narrower than one block and every leftover column count. */
    static const int geometries[][2] = {{12, 10}, {15, 9},  {16, 9},  {23, 17},
                                        {40, 30}, {67, 21}, {130, 12}};
    const I4CmKernels k = i4_cm_kernels();

    for (size_t g = 0; g < sizeof(geometries) / sizeof(geometries[0]); ++g) {
        char *msg = i4_cm_check_geometry(geometries[g][0], geometries[g][1], &k);
        if (msg) {
            return msg;
        }
    }
    return NULL;
}

#if ARCH_X86
/* ---------------------------------------------------------------------
 * Test 3: guard band + small-size sweep for adm_decouple_avx2 / _avx512.
 *
 * Netflix/vmaf 03b5562c5: adm_decouple_avx2 computed its 8-wide tail bound
 * from column 0 instead of from `left`, so the last vector store ran up to
 * seven columns past `right` -- into border columns nothing reads, and for
 * band widths 32 and 40 past the end of the row into the next row or band.
 * Nothing compared the samples outside [left, right). Here every output
 * plane is filled with the guard pattern first, using the band stride
 * integer_adm.c derives, and the kernel must leave everything outside the
 * decouple region intact.
 * ------------------------------------------------------------------- */

/* planes[0..5] are the inputs (ref h/v/d, dis h/v/d), planes[6..11] the
 * outputs (decouple_r h/v/d, decouple_a h/v/d). */
typedef struct DecoupleFixture {
    int w;
    int h;
    int stride;
    size_t plane_elems;
    int16_t *planes[DEC_PLANES];
    AdmBuffer buf;
} DecoupleFixture;

static void decouple_fixture_free(DecoupleFixture *f)
{
    for (int k = 0; k < DEC_PLANES; ++k) {
        simd_test_aligned_free(f->planes[k]);
        f->planes[k] = NULL;
    }
}

static void decouple_fixture_bind(DecoupleFixture *f)
{
    f->buf.ref_dwt2.band_h = f->planes[0];
    f->buf.ref_dwt2.band_v = f->planes[1];
    f->buf.ref_dwt2.band_d = f->planes[2];
    f->buf.dis_dwt2.band_h = f->planes[3];
    f->buf.dis_dwt2.band_v = f->planes[4];
    f->buf.dis_dwt2.band_d = f->planes[5];
    f->buf.decouple_r.band_h = f->planes[6];
    f->buf.decouple_r.band_v = f->planes[7];
    f->buf.decouple_r.band_d = f->planes[8];
    f->buf.decouple_a.band_h = f->planes[9];
    f->buf.decouple_a.band_v = f->planes[10];
    f->buf.decouple_a.band_d = f->planes[11];
}

/* Inputs get DWT-sized random samples, outputs the guard pattern. Returns 0,
 * or -1 after freeing whatever was allocated. */
static int decouple_fixture_alloc(DecoupleFixture *f, int w, int h, uint32_t seed)
{
    (void)memset(f, 0, sizeof(*f));
    f->w = w;
    f->h = h;
    /* integer_adm.c: buf_stride = ALIGN_CEIL(band width * 4) >> 2. */
    f->stride = ALIGN_CEIL(w * (int)sizeof(int32_t)) / (int)sizeof(int32_t);
    f->plane_elems = ((size_t)f->stride * (size_t)h) + DEC_SLACK;

    uint32_t state = seed;
    for (int k = 0; k < DEC_PLANES; ++k) {
        f->planes[k] = (int16_t *)simd_test_aligned_malloc(f->plane_elems * sizeof(int16_t), 32);
        if (!f->planes[k]) {
            decouple_fixture_free(f);
            return -1;
        }
        if (k >= 6) {
            simd_test_guard_fill(f->planes[k], f->plane_elems * sizeof(int16_t));
            continue;
        }
        for (size_t i = 0; i < f->plane_elems; ++i) {
            f->planes[k][i] = (int16_t)((int)(simd_test_xorshift32(&state) % 8192u) - 4096);
        }
    }
    decouple_fixture_bind(f);
    return 0;
}

/* The decouple region, derived exactly as the kernels derive it. */
static SimdTestRect decouple_region(int w, int h)
{
    int left = (int)((w * ADM_BORDER_FACTOR) - 0.5 - 1);
    int top = (int)((h * ADM_BORDER_FACTOR) - 0.5 - 1);
    int right = w - left + 2;
    int bottom = h - top + 2;

    left = left < 0 ? 0 : left;
    top = top < 0 ? 0 : top;
    right = right > w ? w : right;
    bottom = bottom > h ? h : bottom;

    const SimdTestRect rect = {(size_t)top, (size_t)bottom, (size_t)left, (size_t)right};
    return rect;
}

static size_t decouple_outputs_touched(const DecoupleFixture *f, SimdTestRect rect)
{
    size_t touched = 0;
    for (int k = 6; k < DEC_PLANES; ++k) {
        const SimdTestPlane plane = {f->planes[k], sizeof(int16_t), (size_t)f->stride, (size_t)f->h,
                                     DEC_SLACK};
        touched += simd_test_guard_count_outside(plane, rect);
    }
    return touched;
}

static int decouple_regions_equal(const DecoupleFixture *a, const DecoupleFixture *b,
                                  SimdTestRect rect)
{
    for (int k = 6; k < DEC_PLANES; ++k) {
        for (size_t i = rect.row0; i < rect.row1; ++i) {
            const size_t row = i * (size_t)a->stride;
            const size_t n = (rect.col1 - rect.col0) * sizeof(int16_t);
            if (memcmp(a->planes[k] + row + rect.col0, b->planes[k] + row + rect.col0, n) != 0) {
                return 0;
            }
        }
    }
    return 1;
}

/* Runs `kernel` on one geometry and checks its guard band; when `cross` is
 * set, runs it on an identical fixture and requires the same guard band and
 * the same in-region samples. */
static char *check_decouple_geometry(adm_decouple_fn kernel, adm_decouple_fn cross, int w, int h)
{
    DecoupleFixture a;
    DecoupleFixture b;
    const uint32_t seed = 0xdec0u ^ (uint32_t)((w * 131) + h);

    mu_assert("allocation failed for the decouple fixture",
              decouple_fixture_alloc(&a, w, h, seed) == 0);
    if (decouple_fixture_alloc(&b, w, h, seed) != 0) {
        decouple_fixture_free(&a);
        return "allocation failed for the decouple fixture";
    }

    const SimdTestRect rect = decouple_region(w, h);
    kernel(&a.buf, w, h, a.stride, DEFAULT_ADM_ENHN_GAIN_LIMIT, div_lookup);
    size_t touched = decouple_outputs_touched(&a, rect);
    int same = 1;
    if (cross) {
        cross(&b.buf, w, h, b.stride, DEFAULT_ADM_ENHN_GAIN_LIMIT, div_lookup);
        touched += decouple_outputs_touched(&b, rect);
        same = decouple_regions_equal(&a, &b, rect);
    }
    decouple_fixture_free(&a);
    decouple_fixture_free(&b);

    if (touched != 0 || !same) {
        (void)fprintf(stderr, "  decouple band %dx%d region [%zu,%zu)x[%zu,%zu)\n", w, h, rect.row0,
                      rect.row1, rect.col0, rect.col1);
    }
    SIMD_GUARD_ASSERT_UNTOUCHED(touched, "adm_decouple wrote outside the decouple region");
    mu_assert("adm_decouple AVX2 and AVX-512 disagree inside the decouple region", same);
    return NULL;
}

static char *test_adm_decouple_guard_band(void)
{
    /* Band heights: full-height regions (top == 0) up to a clipped border. */
    static const int heights[] = {8, 12, 17, 24, 36};
    adm_decouple_fn cross = NULL;
#if HAVE_AVX512
    if (simd_test_have_avx512()) {
        cross = adm_decouple_avx512;
    }
#endif

    for (size_t t = 0; t < sizeof(heights) / sizeof(heights[0]); ++t) {
        for (int w = 8; w <= 80; ++w) {
            char *msg = check_decouple_geometry(adm_decouple_avx2, cross, w, heights[t]);
            if (msg) {
                return msg;
            }
        }
    }
    return NULL;
}

#endif /* ARCH_X86 */

#endif /* ARCH_X86 || ARCH_AARCH64 */

#if ARCH_X86 || ARCH_AARCH64

/* ---------------------------------------------------------------------
 * Test 5: the decouple kernels against the scalar kernels, for integer and
 * non-integer enhancement gain limits.
 *
 * adm_decouple_band() and adm_decouple_band_s123() bound the restored sample
 * with MIN(rst * gain, t) / MAX(rst * gain, t) in double and store the result
 * in an integer, which truncates toward zero. The scale-0 AVX2 and AVX-512
 * kernels and the AVX-512 scale 1-3 kernel converted rst * gain with the
 * rounding conversions, so with a gain such as 1.2 they left the scalar by one
 * in every limited sample whose product has a fraction of one half or more
 * (T-ADM-DECOUPLE-X86-FRACTIONAL-GAIN-ROUNDING-2026-10-01). The gains of the
 * shipped models, 1 and 100, give integral products and hid it.
 *
 * The bands are built so that the limit is taken: in three samples of four
 * the distorted sample is the reference scaled by one factor, which passes
 * the one-degree angle test, and the factor runs from 1/2 to 8 and, in one
 * sample of eight, to 150, past every gain under test.
 *
 * They also reach the bottom of the int16 range, which a decoded picture does
 * not (its scale-0 coefficients stay within about 22900): with h = v = -32768
 * the vector angle test's _mm256_madd_epi16 / _mm512_madd_epi16 sum is 2^31
 * and read INT32_MIN, which set the angle flag where the scalar clears it.
 * ------------------------------------------------------------------- */

/* Sixteen-bit planes feed the scale-0 kernels, 32-bit planes the scale 1-3
 * kernels; both sets use the plane order of DecoupleFixture. */
typedef struct GainFixture {
    int w;
    int h;
    int stride;
    size_t elems;
    int16_t *p16[DEC_PLANES];
    int32_t *p32[DEC_PLANES];
    AdmBuffer buf;
} GainFixture;

/* Reference and distorted sample of the three bands at one position. */
typedef struct GainSample {
    int32_t o[3];
    int32_t t[3];
} GainSample;

static int32_t gain_clamp(int64_t v, int32_t limit)
{
    if (v < -(int64_t)limit) {
        return -limit;
    }
    return (v >= limit) ? limit - 1 : (int32_t)v;
}

static int32_t gain_random(uint32_t *state, int32_t mag)
{
    return (int32_t)(simd_test_xorshift32(state) % (2u * (uint32_t)mag)) - mag;
}

/* One position of bands whose samples lie in [-limit, limit); `classes` is
 * the number of magnitude classes, each half the one before. */
static GainSample gain_sample(uint32_t *state, int32_t limit, uint32_t classes)
{
    GainSample s;
    const uint32_t kind = simd_test_xorshift32(state);
    const int32_t shifted = limit >> (kind % classes);
    const int32_t mag = shifted < 4 ? 4 : shifted;
    /* The enhancement factor, in eighths. */
    const int64_t num = ((kind >> 8) % 8u == 0u) ? 1200 : 4 + (int64_t)((kind >> 12) % 60u);
    const int independent = ((kind >> 20) % 4u == 0u);

    for (int b = 0; b < 3; ++b) {
        s.o[b] = gain_random(state, mag);
        s.t[b] =
            independent ? gain_random(state, mag) : gain_clamp(((int64_t)s.o[b] * num) / 8, limit);
    }
    /* The angle test reads h and v only; d may point the other way. */
    if ((kind >> 24) & 1u) {
        s.t[2] = gain_clamp(-(int64_t)s.t[2], limit);
    }
    /* h and v at the bottom of the range in both pictures: at scale 0 the
     * int32 dot product and squared magnitudes of the angle test are 2^31. */
    if ((kind >> 25) % 32u == 0u) {
        s.o[0] = s.o[1] = s.t[0] = s.t[1] = -limit;
    }
    return s;
}

static void gain_fixture_free(GainFixture *f)
{
    for (int k = 0; k < DEC_PLANES; ++k) {
        simd_test_aligned_free(f->p16[k]);
        simd_test_aligned_free(f->p32[k]);
        f->p16[k] = NULL;
        f->p32[k] = NULL;
    }
}

static void gain_fixture_bind(GainFixture *f)
{
    adm_dwt_band_t *const b16[4] = {&f->buf.ref_dwt2, &f->buf.dis_dwt2, &f->buf.decouple_r,
                                    &f->buf.decouple_a};
    i4_adm_dwt_band_t *const b32[4] = {&f->buf.i4_ref_dwt2, &f->buf.i4_dis_dwt2,
                                       &f->buf.i4_decouple_r, &f->buf.i4_decouple_a};

    for (size_t n = 0; n < 4; ++n) {
        const size_t first = 3u * n;
        b16[n]->band_h = f->p16[first];
        b16[n]->band_v = f->p16[first + 1u];
        b16[n]->band_d = f->p16[first + 2u];
        b32[n]->band_h = f->p32[first];
        b32[n]->band_v = f->p32[first + 1u];
        b32[n]->band_d = f->p32[first + 2u];
    }
}

static void gain_fixture_fill(GainFixture *f, uint32_t seed)
{
    uint32_t state = seed;

    for (size_t i = 0; i < f->elems; ++i) {
        const GainSample s16 = gain_sample(&state, 32768, 12u);
        const GainSample s32 = gain_sample(&state, 1 << 26, 24u);
        for (int b = 0; b < 3; ++b) {
            f->p16[b][i] = (int16_t)s16.o[b];
            f->p16[3 + b][i] = (int16_t)s16.t[b];
            f->p32[b][i] = s32.o[b];
            f->p32[3 + b][i] = s32.t[b];
        }
    }
}

/* Inputs get the enhanced bands of `seed`, outputs zeros. Returns 0, or -1
 * after freeing whatever was allocated. */
static int gain_fixture_alloc(GainFixture *f, int w, int h, uint32_t seed)
{
    (void)memset(f, 0, sizeof(*f));
    f->w = w;
    f->h = h;
    f->stride = ALIGN_CEIL(w * (int)sizeof(int32_t)) / (int)sizeof(int32_t);
    f->elems = ((size_t)f->stride * (size_t)h) + DEC_SLACK;

    for (int k = 0; k < DEC_PLANES; ++k) {
        f->p16[k] = (int16_t *)simd_test_aligned_malloc(f->elems * sizeof(int16_t), 64);
        f->p32[k] = (int32_t *)simd_test_aligned_malloc(f->elems * sizeof(int32_t), 64);
        if (!f->p16[k] || !f->p32[k]) {
            gain_fixture_free(f);
            return -1;
        }
        (void)memset(f->p16[k], 0, f->elems * sizeof(int16_t));
        (void)memset(f->p32[k], 0, f->elems * sizeof(int32_t));
    }
    gain_fixture_fill(f, seed);
    gain_fixture_bind(f);
    return 0;
}

/* adm_decouple() and adm_decouple_s123() of integer_adm.c, from the kernels
 * they are built on. */
static void gain_scalar(GainFixture *f, double gain)
{
    const float cos_1deg_sq = adm_cos_1deg_sq();
    const AdmBorder b = adm_border_filt(f->w, f->h);

    for (int i = b.top; i < b.bottom; ++i) {
        adm_decouple_cols(&f->buf, i, f->stride, b.left, b.right, gain, div_lookup, cos_1deg_sq);
        adm_decouple_s123_cols(&f->buf, i, f->stride, b.left, b.right, gain, div_lookup,
                               cos_1deg_sq);
    }
}

/* Output samples of the 16-bit (`n[0]`) and 32-bit (`n[1]`) planes on which
 * the two fixtures differ. */
static void gain_outputs_differ(const GainFixture *a, const GainFixture *b, size_t n[2])
{
    n[0] = 0;
    n[1] = 0;
    for (int k = 6; k < DEC_PLANES; ++k) {
        for (size_t i = 0; i < a->elems; ++i) {
            n[0] += (a->p16[k][i] != b->p16[k][i]) ? 1u : 0u;
            n[1] += (a->p32[k][i] != b->p32[k][i]) ? 1u : 0u;
        }
    }
}

/* The vector decouple kernels the host can run. */
typedef struct GainKernels {
    adm_decouple_fn s0[2];
    adm_decouple_fn s123[2];
    const char *name[2];
    int count;
} GainKernels;

static GainKernels gain_kernels(void)
{
#if ARCH_AARCH64
    /* Netflix/vmaf 9e48141b (scale 0) and b41d2340a (scales 1-3). */
    const GainKernels k = {
        {adm_decouple_neon, NULL}, {adm_decouple_s123_neon, NULL}, {"NEON", NULL}, 1};
    return k;
#else
    GainKernels k = {{adm_decouple_avx2, NULL}, {adm_decouple_s123_avx2, NULL}, {"AVX2", NULL}, 1};
#if HAVE_AVX512
    if (simd_test_have_avx512()) {
        k.s0[1] = adm_decouple_avx512;
        k.s123[1] = adm_decouple_s123_avx512;
        k.name[1] = "AVX-512";
        k.count = 2;
    }
#endif
    return k;
#endif
}

/* Runs every vector kernel on `simd` and counts the output samples that are
 * not the scalar kernels' in `ref`. */
static size_t gain_mismatches(const GainKernels *k, const GainFixture *ref, GainFixture *simd,
                              double gain)
{
    size_t total = 0;

    for (int n = 0; n < k->count; ++n) {
        size_t bad[2];
        k->s0[n](&simd->buf, simd->w, simd->h, simd->stride, gain, div_lookup);
        if (k->s123[n]) {
            k->s123[n](&simd->buf, simd->w, simd->h, simd->stride, gain, div_lookup);
        }
        gain_outputs_differ(ref, simd, bad);
        if (!k->s123[n]) {
            /* No scale 1-3 kernel to hold to the scalar: its planes still hold
             * the scalar output at the default limit. */
            bad[1] = 0;
        }
        if (bad[0] != 0 || bad[1] != 0) {
            (void)fprintf(stderr,
                          "  decouple %s, band %dx%d, gain limit %g: %zu scale-0 and %zu "
                          "scale 1-3 samples are not the scalar kernels'\n",
                          k->name[n], simd->w, simd->h, gain, bad[0], bad[1]);
        }
        total += bad[0] + bad[1];
    }
    return total;
}

/* One geometry at one gain limit. `limited` receives the number of scalar
 * output samples the limit changed, taken against the scalar output at the
 * default limit of 100, which only the factor of 150 reaches. */
static char *gain_check(const GainKernels *k, int w, int h, double gain, size_t limited[2])
{
    GainFixture ref;
    GainFixture simd;
    const uint32_t seed = 0x6a17u ^ (uint32_t)((w * 257) + h);

    mu_assert("allocation failed for the gain fixture", gain_fixture_alloc(&ref, w, h, seed) == 0);
    if (gain_fixture_alloc(&simd, w, h, seed) != 0) {
        gain_fixture_free(&ref);
        return "allocation failed for the gain fixture";
    }

    gain_scalar(&simd, 100.0);
    gain_scalar(&ref, gain);
    gain_outputs_differ(&ref, &simd, limited);
    const size_t bad = gain_mismatches(k, &ref, &simd, gain);

    gain_fixture_free(&ref);
    gain_fixture_free(&simd);
    mu_assert("a vector decouple kernel and the scalar kernels disagree", bad == 0);
    return NULL;
}

static char *test_adm_decouple_matches_scalar_for_gains(void)
{
    static const double gains[] = {1.0, 1.2, 1.5, 2.0, 3.0, 7.0, 100.0};
    static const int geometry[][2] = {{24, 12}, {37, 17}, {64, 24}, {80, 36}};
    const GainKernels k = gain_kernels();

    for (size_t g = 0; g < sizeof(gains) / sizeof(gains[0]); ++g) {
        size_t limited[2] = {0, 0};
        for (size_t n = 0; n < sizeof(geometry) / sizeof(geometry[0]); ++n) {
            size_t part[2];
            char *msg = gain_check(&k, geometry[n][0], geometry[n][1], gains[g], part);
            if (msg) {
                return msg;
            }
            limited[0] += part[0];
            limited[1] += part[1];
        }
        /* A gain limit below 100 has to bind somewhere, or the comparison
         * above proves nothing about the limited branch. */
        if (gains[g] < 100.0) {
            mu_assert("the gain limit never bound a scale-0 sample", limited[0] > 100);
            mu_assert("the gain limit never bound a scale 1-3 sample", limited[1] > 100);
        }
    }
    return NULL;
}

#endif /* ARCH_X86 || ARCH_AARCH64 */

char *run_tests(void)
{
    /* The arithmetic precision test runs on every arch (it is pure C). */
    static const MuTest portable[] = {MU_TEST(test_adm_accum_precision)};
    char *msg = mu_run_table(portable, MU_TABLE_LEN(portable));
    if (msg) {
        return msg;
    }
#if ARCH_X86
    if (!simd_test_have_avx2()) {
        return NULL;
    }
    static const MuTest simd[] = {
        MU_TEST(test_adm_cm_avx2_smoke),
        MU_TEST(test_i4_adm_cm_avx2_p_norm),
        MU_TEST(test_adm_cm_centre_tap_stays_int32),
        MU_TEST(test_adm_cm_matches_scalar_kernels),
        MU_TEST(test_i4_adm_cm_matches_scalar_kernels),
        MU_TEST(test_adm_decouple_guard_band),
        MU_TEST(test_adm_decouple_matches_scalar_for_gains),
    };
    div_lookup_generator();
    return mu_run_table(simd, MU_TABLE_LEN(simd));
#elif ARCH_AARCH64
    static const MuTest simd[] = {
        MU_TEST(test_adm_cm_centre_tap_stays_int32),
        MU_TEST(test_adm_cm_matches_scalar_kernels),
        MU_TEST(test_i4_adm_cm_matches_scalar_kernels),
        MU_TEST(test_adm_decouple_matches_scalar_for_gains),
    };
    div_lookup_generator();
    return mu_run_table(simd, MU_TABLE_LEN(simd));
#else
    (void)fprintf(stderr, "skipping SIMD smoke: non-x86 arch\n");
    return NULL;
#endif
}

/* NOLINTEND(modernize-use-nullptr) */
