/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  T-ADM-AIM-BARTEN-SCALE-TERM-WRAP-2026-10-01 / ADR-1472: the fixed-point
 *  CSF weights of the integer ADM pipeline are limited by the contrast-masking
 *  cube, not by their storage.
 *
 *  Per sample the reduction squares the excess of the weighted wavelet
 *  coefficient over its threshold and narrows the square to int32. With a
 *  weight limit of 2^30 at scales 1..3 (ADR-1325) a Barten-mode weight could
 *  be large enough for that square, and for the weighted coefficient itself,
 *  to leave int32: the cube turned negative or small, `integer_adm2` was wrong
 *  without any message (0.587 for 0.784 on the 1 px checkerboard) or the
 *  frame failed with a NaN numerator (10 px checkerboard).
 *
 *    1. positive: the largest coefficient a scale can produce, derived here
 *       from the wavelet taps, lies under the constant of
 *       adm_csf_fixed_point.h and within one percent of it;
 *    2. boundary: a weight just under adm_csf_fixed_limit() keeps the square
 *       of the worst sample in int32;
 *    3. negative: a weight two percent over it does not, so the limits are as
 *       wide as the arithmetic allows;
 *    4. positive: adm_csf_fixed_scale() brings every weight of a sweep over
 *       seven decades and three band ratios under its limit with the smallest
 *       exponent that does;
 *    5. end to end: on frames built from the sign pattern of the composite
 *       wavelet filter, which drive one coefficient to its largest value,
 *       the `adm` extractor in Barten mode scores and agrees with
 *       `float_adm`. Before the fix four of the five failed and the fifth
 *       returned 0.862 for 1.0.
 *
 *  ADR-1917 / T-ADM-SCALE0-CSF-FLT-INT16-WRAP-2026-10-05: at scale 0 the
 *  int16 1/30 magnitude of the CSF stage binds the horizontal and vertical
 *  weight before the cube does (43900 against 46603.4). Its constants are
 *  derived again from the stage's arithmetic and the taps; a weight under
 *  the limit keeps the magnitude in int16 and one two percent over it does
 *  not; a Barten configuration whose scale-0 weight was 46012 under the old
 *  limit is now normalised once more. On master the magnitude test and the
 *  normalisation test fail.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "feature/adm_csf_fixed_point.h"
#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "feature/integer_adm.h"
#include "feature/integer_adm_kernels.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this test mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#define FRAME_DIM (256)
#define NUM_SCALES (4)
/* Integer and float ADM agree to 2.4e-6 or better on the frames below. */
#define FLOAT_TOLERANCE (1.0e-4)

/* Fixed-point format of a detail band per scale: the scale-0 wavelet leaves
 * 2^14 (8 + 15 + 15 - 8 - 16 bits), scales 1..3 add 2^15, 2^-2 and 2^-1
 * (i4_dwt2_round(): shifts of 0 + 15, 16 + 16 and 16 + 15 against 2^30). */
static const double BAND_FORMAT[NUM_SCALES] = {16384.0, 536870912.0, 134217728.0, 67108864.0};
static const double BAND_MAX[NUM_SCALES] = {ADM_DWT_BAND_MAX_SCALE0, ADM_DWT_BAND_MAX_SCALE1,
                                            ADM_DWT_BAND_MAX_SCALE2, ADM_DWT_BAND_MAX_SCALE3};

/* One wavelet level as a real-valued map of `n` samples to (n + 1) / 2: taps
 * at 2i - 1 .. 2i + 2, mirrored as dwt2_src_indices_filt() mirrors them. */
static void dwt_level(const double *in, int n, const int16_t *taps, double *out)
{
    const int half = (n + 1) / 2;
    for (int i = 0; i < half; i++) {
        double acc = 0.0;
        for (int k = 0; k < 4; k++) {
            int idx = 2 * i - 1 + k;
            idx = idx < 0 ? 1 : idx;
            idx = idx >= n ? 2 * n - idx - 1 : idx;
            acc += (double)taps[k] / 32768.0 * in[idx];
        }
        out[i] = acc;
    }
}

/* Response of the centre coefficient of `scale` to a unit sample at `pos`:
 * `scale` low-pass levels, then one level of `last`. */
static double centre_response(int scale, const int16_t *last, int pos)
{
    double a[FRAME_DIM] = {0.0};
    double b[FRAME_DIM] = {0.0};
    int n = FRAME_DIM;

    a[pos] = 1.0;
    for (int level = 0; level < scale; level++) {
        dwt_level(a, n, dwt2_db2_coeffs_lo, b);
        n = (n + 1) / 2;
        (void)memcpy(a, b, (size_t)n * sizeof(a[0]));
    }
    dwt_level(a, n, last, b);
    return b[((n + 1) / 2) / 2];
}

/* The composite filter of the centre coefficient, and its absolute sum. */
static double composite(int scale, const int16_t *last, double g[FRAME_DIM])
{
    double abs_sum = 0.0;
    for (int pos = 0; pos < FRAME_DIM; pos++) {
        g[pos] = centre_response(scale, last, pos);
        abs_sum += fabs(g[pos]);
    }
    return abs_sum;
}

/* Least upper bound of a detail coefficient of `scale`: the centred pixel
 * lies in [-1/2, 1/2), so half the absolute sum of the two-dimensional
 * filter, in the band's format. The larger of the h / v and the d band. */
static double band_supremum(int scale)
{
    double g[FRAME_DIM];
    const double lo = composite(scale, dwt2_db2_coeffs_lo, g);
    const double hi = composite(scale, dwt2_db2_coeffs_hi, g);
    const double hv = 0.5 * lo * hi;
    const double d = 0.5 * hi * hi;
    return BAND_FORMAT[scale] * (hv > d ? hv : d);
}

/* positive: the header's constants cover the taps and stay within 1 %. */
static char *test_band_bounds_follow_the_filter_taps(void)
{
    for (int scale = 0; scale < NUM_SCALES; scale++) {
        const double sup = band_supremum(scale);
        mu_assert("a band bound is below what the wavelet taps can produce",
                  BAND_MAX[scale] >= sup);
        mu_assert("a band bound is more than one percent above the taps' bound",
                  BAND_MAX[scale] <= sup * 1.01);
    }
    return NULL;
}

/* True when the reduction's `(v * v + round) >> shift` fits int32. The
 * excess is never negative, so the check runs on unsigned values. */
static bool square_fits(uint64_t v, unsigned shift)
{
    const uint64_t square = ((v * v) + (UINT64_C(1) << (shift - 1u))) >> shift;
    return square <= (uint64_t)INT32_MAX;
}

/* The largest excess the reduction can form at `scale` under `weight`. */
static uint64_t worst_excess(int scale, double weight)
{
    const uint64_t band = (uint64_t)BAND_MAX[scale];
    const uint64_t w = (uint64_t)weight;
    if (scale == 0) {
        return band * w;
    }
    /* i4_adm_cm_scale(): + 2^27, >> 28; then a threshold of -27 (ADR-0155
     * rounding term). */
    return (((band * w) + UINT64_C(134217728)) / UINT64_C(268435456)) + 27u;
}

/* Bits the square is shifted by: 29 for the scale-0 h / v bands, else 30. */
static unsigned square_shift(int scale, int band)
{
    return (scale == 0 && band != 2) ? 29u : 30u;
}

/* The CSF stage's int16 1/30 magnitude of a scale-0 band sample `i16`. */
static int64_t csf_magnitude(int64_t i16)
{
    return ((ADM_FIX_ONE_BY_30 * i16) + 2048) >> 12;
}

/* True when the 1/30 magnitude of the largest scale-0 band, weighted by the
 * integer part of `weight` in the h / v band (shift 15) or the d band (shift
 * 17), fits int16. Holds trivially at scales 1..3, whose CSF stage is int32. */
static bool magnitude_fits(int scale, int band, double weight)
{
    if (scale != 0) {
        return true;
    }
    const int64_t w = (int64_t)weight;
    const int64_t reach = (int64_t)ADM_DWT_BAND_REACH_SCALE0;
    const int64_t i16 = band == 2 ? ((w * reach) + 65535) >> 17 : ((w * reach) + 16384) >> 15;
    return i16 <= INT16_MAX && csf_magnitude(i16) <= INT16_MAX;
}

/* positive: ADM_CSF_FLT_I16_LIMIT is the first |i16| whose magnitude passes
 * INT16_MAX, and ADM_DWT_BAND_REACH_SCALE0 covers the taps' bound plus the
 * rounding of the two passes (half a unit each, the first through the second
 * pass's gain: the absolute tap sum over 2^16). */
static char *test_scale0_magnitude_constants_follow_the_stage(void)
{
    int64_t first = 0;
    while (csf_magnitude(first) <= INT16_MAX) {
        first++;
    }
    mu_assert("ADM_CSF_FLT_I16_LIMIT is not the first |i16| past INT16_MAX",
              (double)first == ADM_CSF_FLT_I16_LIMIT);
    double tap_sum = 0.0;
    for (int k = 0; k < 4; k++) {
        tap_sum += fabs((double)dwt2_db2_coeffs_lo[k]);
    }
    const double reach = floor(band_supremum(0) + 0.5 + (0.5 * tap_sum / 65536.0));
    mu_assert("ADM_DWT_BAND_REACH_SCALE0 is below the band the integer DWT can reach",
              ADM_DWT_BAND_REACH_SCALE0 >= reach);
    mu_assert("ADM_DWT_BAND_REACH_SCALE0 is above the taps' bound plus one",
              ADM_DWT_BAND_REACH_SCALE0 <= band_supremum(0) + 1.0);
    mu_assert("the scale-0 h / v limit is not 43900", adm_csf_fixed_limit(0, 0) == 43900.0);
    mu_assert("the scale-0 h / v limit is not 43900", adm_csf_fixed_limit(0, 1) == 43900.0);
    return NULL;
}

/* boundary: just under its limit a weight keeps the worst square in int32. */
static char *test_a_weight_under_the_limit_keeps_the_square_in_int32(void)
{
    for (int scale = 0; scale < NUM_SCALES; scale++) {
        for (int band = 0; band < 3; band++) {
            const double limit = adm_csf_fixed_limit(scale, band);
            const double weight = floor(nextafter(limit, 0.0));
            mu_assert("a weight limit exceeds the storage of the weight",
                      limit <= (scale == 0 ? 65536.0 : 1073741824.0));
            mu_assert("the worst sample under the largest weight leaves int32 when squared",
                      square_fits(worst_excess(scale, weight), square_shift(scale, band)));
            mu_assert("the CSF magnitude of the worst sample under the largest weight leaves int16",
                      magnitude_fits(scale, band, weight));
        }
    }
    return NULL;
}

/* negative: two percent over the limit the square leaves int32 at scales 1..3
 * and the CSF magnitude leaves int16 at scale 0, where it binds first (the
 * square still fits there up to 46603). The scale-0 diagonal weight is bound
 * by its uint16_t storage and has no such point. */
static char *test_a_weight_over_the_limit_leaves_int32(void)
{
    const double over0 = ceil(adm_csf_fixed_limit(0, 0) * 1.02);
    mu_assert("a scale-0 weight two percent over the limit keeps its CSF magnitude in int16",
              !magnitude_fits(0, 0, over0));
    mu_assert("the cube no longer allows the scale-0 weight two percent over the limit",
              square_fits(worst_excess(0, over0), square_shift(0, 0)));
    for (int scale = 1; scale < NUM_SCALES; scale++) {
        const double weight = ceil(adm_csf_fixed_limit(scale, 0) * 1.02);
        mu_assert("a weight two percent over the limit still fits: the limit is not tight",
                  !square_fits(worst_excess(scale, weight), square_shift(scale, 0)));
    }
    return NULL;
}

/* T-ADM-SCALE0-CSF-FLT-INT16-WRAP-2026-10-05: Barten mode with
 * adm_csf_scale=1.16 and adm_csf_diag_scale=0.3 gave a scale-0 h / v weight of
 * 46012 after six halvings, inside the cube's budget but past the magnitude's.
 * Now it takes a seventh. */
static char *test_a_scale0_weight_past_the_magnitude_limit_is_halved_again(void)
{
    const AdmCsfFactors f = adm_csf_factors(0, 3.0, 1080, ADM_CSF_MODE_BARTEN, 1.16, 0.3);
    const float rfactor1[3] = {f.factor1, f.factor1, f.factor2};
    double fixed[3];
    uint32_t shift = 0u;
    mu_assert("adm_csf_fixed_scale rejected a Barten weight",
              adm_csf_fixed_scale(0, rfactor1, 3.0, 1080, ADM_CSF_MODE_BARTEN, fixed, &shift) == 0);
    mu_assert("the scale-0 weight was not halved past the magnitude limit", shift == 7u);
    mu_assert("the halved scale-0 weight is not below 43900", fixed[0] < 43900.0);
    mu_assert("the halved scale-0 weight keeps no CSF magnitude in int16",
              magnitude_fits(0, 0, fixed[0]));
    return NULL;
}

/* The checks of one normalised scale; NULL when they hold. */
static char *check_normalised(int scale, const double fixed[3], uint32_t shift)
{
    bool minimal = shift == 0u;
    for (int band = 0; band < 3; band++) {
        const double limit = adm_csf_fixed_limit(scale, band);
        mu_assert("a normalised weight is not below its limit", fixed[band] < limit);
        mu_assert("the worst sample under a normalised weight leaves int32 when squared",
                  square_fits(worst_excess(scale, fixed[band]), square_shift(scale, band)));
        mu_assert("the CSF magnitude under a normalised weight leaves int16",
                  magnitude_fits(scale, band, fixed[band]));
        minimal = minimal || (fixed[band] * 2.0 >= limit);
    }
    mu_assert("the normalisation exponent is larger than the limits require", minimal);
    return NULL;
}

/* positive: seven decades of weights. The band ratios cover the diagonal
 * weight equal to the horizontal one, a quarter of it (where the scale-0
 * h / v limit binds), and a vertical weight above the horizontal one. Mode 1
 * keeps scale 0 off its table. */
static char *check_sweep(int scale, const float ratio[3])
{
    for (int step = -35; step <= 35; step++) {
        const float factor = powf(10.0f, (float)step / 10.0f);
        const float rfactor1[3] = {factor * ratio[0], factor * ratio[1], factor * ratio[2]};
        double fixed[3];
        uint32_t shift = 0u;
        mu_assert("adm_csf_fixed_scale rejected a finite positive weight",
                  adm_csf_fixed_scale(scale, rfactor1, 3.0, 1080, 1, fixed, &shift) == 0);
        char *msg = check_normalised(scale, fixed, shift);
        if (msg) {
            return msg;
        }
    }
    return NULL;
}

static char *test_normalisation_brings_every_weight_under_its_limit(void)
{
    static const float BAND_RATIO[3][3] = {
        {1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 0.25f},
        {0.5f, 1.0f, 0.25f},
    };
    for (int scale = 0; scale < NUM_SCALES; scale++) {
        for (int r = 0; r < 3; r++) {
            char *msg = check_sweep(scale, BAND_RATIO[r]);
            if (msg) {
                return msg;
            }
        }
    }
    return NULL;
}

/* One end-to-end case: the frame that maximises the centre coefficient of
 * `scale` in the band with a high-pass vertical filter and, for `diagonal`,
 * a high-pass horizontal one too. */
typedef struct Case {
    int scale;
    bool diagonal;
    bool inverted_ref; /* reference of opposite polarity, else the same frame */
    const char *csf_scale;
    const char *csf_diag_scale;
    const char *int_adm2;
    const char *int_aim;
    const char *float_adm2;
    const char *float_aim;
} Case;

static int alloc_frame(VmafPicture *pic, const Case *c, bool invert)
{
    double gv[FRAME_DIM];
    double gh[FRAME_DIM];
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, FRAME_DIM, FRAME_DIM);
    if (err) {
        return err;
    }
    (void)composite(c->scale, dwt2_db2_coeffs_hi, gv);
    (void)composite(c->scale, c->diagonal ? dwt2_db2_coeffs_hi : dwt2_db2_coeffs_lo, gh);

    uint8_t *luma = (uint8_t *)pic->data[0];
    for (int y = 0; y < FRAME_DIM; y++) {
        for (int x = 0; x < FRAME_DIM; x++) {
            const double g = invert ? -(gv[y] * gh[x]) : (gv[y] * gh[x]);
            luma[(ptrdiff_t)y * pic->stride[0] + x] = g > 0.0 ? 255u : (g < 0.0 ? 0u : 128u);
        }
    }
    for (unsigned p = 1u; p < 3u; p++) {
        for (unsigned row = 0u; row < pic->h[p]; row++) {
            (void)memset((uint8_t *)pic->data[p] + (ptrdiff_t)row * pic->stride[p], 128, pic->w[p]);
        }
    }
    return 0;
}

static int set_options(VmafDictionary **dict, const Case *c)
{
    int err = vmaf_dictionary_set(dict, "adm_csf_mode", "1", 0);
    if (!err && c->csf_scale) {
        err = vmaf_dictionary_set(dict, "adm_csf_scale", c->csf_scale, 0);
    }
    if (!err && c->csf_diag_scale) {
        err = vmaf_dictionary_set(dict, "adm_csf_diag_scale", c->csf_diag_scale, 0);
    }
    return err;
}

/* Run extractor `name` over the case's frame pair and read two scores. */
static int extract_pair(const char *name, const Case *c, const char *const keys[2],
                        double scores[2])
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(name);
    VmafDictionary *dict = NULL;
    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    VmafPicture ref;
    VmafPicture dist;

    if (!fex || set_options(&dict, c)) {
        (void)vmaf_dictionary_free(&dict);
        return -EINVAL;
    }
    /* create() takes ownership of `dict`, and frees it on failure. */
    int err = vmaf_feature_extractor_context_create(&ctx, fex, dict);
    if (err) {
        return err;
    }
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, FRAME_DIM, FRAME_DIM);
    err = err ? err : vmaf_feature_collector_init(&fc);
    err = err ? err : alloc_frame(&ref, c, c->inverted_ref);
    if (!err) {
        err = alloc_frame(&dist, c, false);
        err =
            err ? err : vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
        for (unsigned i = 0u; !err && i < 2u; i++) {
            err = vmaf_feature_collector_get_score(fc, keys[i], &scores[i], 0);
        }
        (void)vmaf_picture_unref(&ref);
        (void)vmaf_picture_unref(&dist);
    }
    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    if (fc) {
        vmaf_feature_collector_destroy(fc);
    }
    return err;
}

static char *check_case(const Case *c)
{
    const char *const int_keys[2] = {c->int_adm2, c->int_aim};
    const char *const float_keys[2] = {c->float_adm2, c->float_aim};
    double fixed_point[2] = {NAN, NAN};
    double reference[2] = {NAN, NAN};

    mu_assert("float_adm did not score an adversarial frame",
              extract_pair("float_adm", c, float_keys, reference) == 0);
    mu_assert("adm did not score an adversarial frame in Barten mode",
              extract_pair("adm", c, int_keys, fixed_point) == 0);
    mu_assert("integer adm2 is not finite", isfinite(fixed_point[0]));
    mu_assert("integer aim is not finite", isfinite(fixed_point[1]));
    mu_assert("integer adm2 leaves float_adm's value on an adversarial frame",
              fabs(fixed_point[0] - reference[0]) <= FLOAT_TOLERANCE);
    mu_assert("integer aim leaves float_adm's value on an adversarial frame",
              fabs(fixed_point[1] - reference[1]) <= FLOAT_TOLERANCE);
    return NULL;
}

/* end to end: scales 1..3 at the default Barten scale, and scale 0 with a
 * horizontal weight far above the diagonal one, which is where the scale-0
 * h / v limit binds. */
static char *test_adversarial_frames_score_in_barten_mode(void)
{
    static const Case CASES[6] = {
        {1, true, true, NULL, NULL, "integer_adm2_csf_1", "integer_aim_csf_1", "adm2_csf_1",
         "aim_csf_1"},
        {2, true, true, NULL, NULL, "integer_adm2_csf_1", "integer_aim_csf_1", "adm2_csf_1",
         "aim_csf_1"},
        {3, true, true, NULL, NULL, "integer_adm2_csf_1", "integer_aim_csf_1", "adm2_csf_1",
         "aim_csf_1"},
        {1, false, false, NULL, NULL, "integer_adm2_csf_1", "integer_aim_csf_1", "adm2_csf_1",
         "aim_csf_1"},
        {0, false, false, "1.4", "0.3", "integer_adm2_scfd_0.3_csf_1_scf_1.4",
         "integer_aim_scfd_0.3_csf_1_scf_1.4", "adm2_scfd_0.3_csf_1_scf_1.4",
         "aim_scfd_0.3_csf_1_scf_1.4"},
        /* Scale-0 h weight 46,012 after normalisation: inside the cube's budget
         * (46,603) but past 43,900, where the 1/30 magnitude of the largest
         * band leaves int16 (T-ADM-SCALE0-CSF-FLT-INT16-WRAP-2026-10-05). */
        {0, false, false, "1.16", "0.3", "integer_adm2_scfd_0.3_csf_1_scf_1.16",
         "integer_aim_scfd_0.3_csf_1_scf_1.16", "adm2_scfd_0.3_csf_1_scf_1.16",
         "aim_scfd_0.3_csf_1_scf_1.16"},
    };
    for (unsigned i = 0u; i < 6u; i++) {
        char *msg = check_case(&CASES[i]);
        if (msg) {
            return msg;
        }
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_band_bounds_follow_the_filter_taps);
    mu_run_test(test_scale0_magnitude_constants_follow_the_stage);
    mu_run_test(test_a_weight_under_the_limit_keeps_the_square_in_int32);
    mu_run_test(test_a_weight_over_the_limit_leaves_int32);
    mu_run_test(test_a_scale0_weight_past_the_magnitude_limit_is_halved_again);
    mu_run_test(test_normalisation_brings_every_weight_under_its_limit);
    mu_run_test(test_adversarial_frames_score_in_barten_mode);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
