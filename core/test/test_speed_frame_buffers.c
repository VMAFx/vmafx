/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Regression tests for the frame buffers of the CPU SpEED extractors and for
 *  what filter_and_downscale() reads out of them.
 *
 *  speed_temporal at speed_prescale above 1
 *  (T-SPEED-TEMPORAL-PRESCALE-UP-OVERFLOW-2026-09-30, Netflix/vmaf#1626).
 *  filter_and_downscale() resamples each frame buffer in place at the
 *  prescaled size, which is taller than the source, but the buffers were
 *  sized with the source height.
 *    positive  prescale 1.5 and 2.0 extract three frames with finite scores;
 *    boundary  prescale 1.0 (the default path) and 4.0 (the option maximum);
 *    negative  4.5 is refused when the context is created.
 *
 *  speed_chroma at odd frame sizes (T-SPEED-CHROMA-ODD-SIZE-OVERFLOW-2026-09-30).
 *  vmaf_picture_alloc() rounds an odd chroma extent up and picture_copy()
 *  copies all of it, but the buffers were sized with the extent rounded down.
 *    positive  4:2:0 with an odd width, an odd height and both, 4:2:2 with an
 *              odd width, all extract with finite scores;
 *    boundary  4:2:0 160x160 (even; chroma 80x80, the smallest SpEED accepts)
 *              and 159x159 (chroma 80x80 only when rounded up);
 *    negative  4:2:0 157x157 (chroma 79x79) is refused, not overrun.
 *
 *  A prescale within 1e-3 of 1 that still changes the plane size
 *  (T-SPEED-PRESCALE-NEAR-ONE-SKIPS-RESAMPLE-2026-09-30). The resample was
 *  skipped, so a plane that grew was filtered over a column the buffer never
 *  received and a plane that shrank was cropped. The resample depends only on
 *  the two sizes, so two prescales that give the same scaled size, one inside
 *  the identity tolerance and one outside it, must score bit for bit alike.
 *    growing    speed_temporal 576 -> 577 columns, 1.0009 against 1.0015;
 *    shrinking  speed_chroma (4:4:4, bilinear) 577 -> 576, 0.9991 against 0.998.
 *
 *  Under AddressSanitizer (the ASan job in sanitizers.yml) every positive
 *  overflow case fails on the old code with a heap-buffer-overflow. Without a
 *  sanitizer the prescale overruns crashed when measured (SIGSEGV at 1.5,
 *  SIGABRT at 2.0 and 4.0) but the odd-size ones, a row or a column of floats,
 *  did not, so those rely on the ASan job. The 159x159 boundary case and the
 *  two near-one cases fail on the old code in any build: it refused the plane,
 *  and the paired scores differ.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "test.h"
#include "mu_table.h"

#include "dict.h"
#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "feature/feature_name.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr) -- ADR-1138: retain NULL for Windows C
 * support and upstream-compatible C test conventions. */

#define FB_FRAMES (3u)

typedef struct FbCase {
    const char *extractor;
    const char *prescale; /* speed_prescale option value, NULL for the default */
    const char *method;   /* speed_prescale_method option value, NULL for the default */
    enum VmafPixelFormat pix_fmt;
    unsigned w;
    unsigned h;
} FbCase;

static const char TEMPORAL_FEATURE[] = "Speed_temporal_feature_speed_temporal_score";
static const char CHROMA_U_FEATURE[] = "Speed_chroma_feature_speed_chroma_u_score";
static const char CHROMA_V_FEATURE[] = "Speed_chroma_feature_speed_chroma_v_score";
static const char CHROMA_FEATURE[] = "Speed_chroma_feature_speed_chroma_uv_score";

/* 8-bit picture whose three planes are LCG noise. */
static int alloc_noise_pic(VmafPicture *pic, const FbCase *c, uint32_t seed)
{
    const int err = vmaf_picture_alloc(pic, c->pix_fmt, 8, c->w, c->h);
    if (err)
        return err;
    uint32_t state = seed;
    for (size_t p = 0; p < 3u; p++) {
        uint8_t *data = pic->data[p];
        const size_t stride = (size_t)pic->stride[p];
        for (size_t i = 0; i < pic->h[p]; i++) {
            for (size_t j = 0; j < pic->w[p]; j++) {
                state = state * 1664525u + 1013904223u;
                data[i * stride + j] = (uint8_t)(state >> 24);
            }
        }
    }
    return 0;
}

static int extract_frame(VmafFeatureExtractorContext *ctx, VmafFeatureCollector *fc,
                         const FbCase *c, unsigned index)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = alloc_noise_pic(&ref, c, (2u * index) + 1u);
    if (err)
        return err;
    err = alloc_noise_pic(&dist, c, (2u * index) + 2u);
    if (err) {
        const int err_ref = vmaf_picture_unref(&ref);
        return err_ref ? err_ref : err;
    }
    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, index, fc);
    const int err_ref = vmaf_picture_unref(&ref);
    const int err_dist = vmaf_picture_unref(&dist);
    if (err)
        return err;
    return err_ref ? err_ref : err_dist;
}

/* Every frame, including index 0 (which speed_temporal emits as 0.0), must carry
 * a finite score under the option-suffixed feature name. The scores are copied
 * to `out` when it is not NULL. */
static char *read_scores(const VmafFeatureExtractorContext *ctx, VmafFeatureCollector *fc,
                         const char *feature, double out[FB_FRAMES])
{
    char *name = vmaf_feature_name_from_options(feature, ctx->fex->options, ctx->fex->priv);
    mu_assert("feature name allocation failed", name != NULL);
    char *fail = NULL;
    for (unsigned i = 0; i < FB_FRAMES && !fail; i++) {
        double score = NAN;
        const int err = vmaf_feature_collector_get_score(fc, name, &score, i);
        if (err) {
            fail = "score missing from the collector";
        } else if (!isfinite(score)) {
            fail = "score is not finite";
        } else if (out) {
            out[i] = score;
        }
    }
    free(name);
    return fail;
}

static int set_option(VmafDictionary **opts, const char *key, const char *value)
{
    return value ? vmaf_dictionary_set(opts, key, value, 0) : 0;
}

/* Creates the context; on failure *ctx is NULL and the options are released. */
static int create_context(const FbCase *c, VmafFeatureExtractorContext **ctx)
{
    *ctx = NULL;
    const VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(c->extractor);
    if (!fex)
        return -EINVAL;
    VmafDictionary *opts = NULL;
    int err = set_option(&opts, "speed_prescale", c->prescale);
    if (!err)
        err = set_option(&opts, "speed_prescale_method", c->method);
    if (!err)
        err = vmaf_feature_extractor_context_create(ctx, fex, opts);
    if (err && opts) {
        const int err_free = vmaf_dictionary_free(&opts);
        return err_free ? err_free : err;
    }
    return err;
}

/* Runs FB_FRAMES frames and reads `feature` into `scores` (may be NULL).
 * *extract_err receives the first extraction error, so a negative case can
 * tell a refused geometry from a harness failure. */
static char *run_case(const FbCase *c, const char *feature, int *extract_err,
                      double scores[FB_FRAMES])
{
    VmafFeatureExtractorContext *ctx = NULL;
    mu_assert("extractor context creation failed", create_context(c, &ctx) == 0);
    VmafFeatureCollector *fc = NULL;
    if (vmaf_feature_collector_init(&fc)) {
        const int err_destroy = vmaf_feature_extractor_context_destroy(ctx);
        mu_assert("extractor context destroy failed", err_destroy == 0);
        return "feature collector allocation failed";
    }
    *extract_err = 0;
    for (unsigned i = 0; i < FB_FRAMES && !*extract_err; i++)
        *extract_err = extract_frame(ctx, fc, c, i);
    char *fail = *extract_err ? NULL : read_scores(ctx, fc, feature, scores);

    const int err_close = vmaf_feature_extractor_context_close(ctx);
    const int err_destroy = vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    if (fail)
        return fail;
    mu_assert("extractor close failed", *extract_err || err_close == 0);
    mu_assert("extractor context destroy failed", err_destroy == 0);
    return NULL;
}

static char *expect_scores(const FbCase *c, const char *feature)
{
    int extract_err = 0;
    mu_assert_msg(run_case(c, feature, &extract_err, NULL));
    mu_assert("extraction failed", extract_err == 0);
    return NULL;
}

/* Both cases scale to the same size, so filter_and_downscale() must resample
 * both and every frame of `feature` must match exactly. */
static char *expect_same_scores(const FbCase *a, const FbCase *b, const char *feature)
{
    double score_a[FB_FRAMES] = {0.0};
    double score_b[FB_FRAMES] = {0.0};
    int extract_err = 0;
    mu_assert_msg(run_case(a, feature, &extract_err, score_a));
    mu_assert("extraction failed", extract_err == 0);
    mu_assert_msg(run_case(b, feature, &extract_err, score_b));
    mu_assert("extraction failed", extract_err == 0);
    for (unsigned i = 0; i < FB_FRAMES; i++) {
        mu_assert("same scaled size, different score: the resample was skipped",
                  score_a[i] == score_b[i]);
    }
    return NULL;
}

/* speed_temporal: positive */

static char *test_temporal_prescale_1_5_extracts_three_frames(void)
{
    const FbCase c = {"speed_temporal", "1.5", NULL, VMAF_PIX_FMT_YUV420P, 576u, 324u};
    return expect_scores(&c, TEMPORAL_FEATURE);
}

static char *test_temporal_prescale_2_0_extracts_three_frames(void)
{
    const FbCase c = {"speed_temporal", "2.0", NULL, VMAF_PIX_FMT_YUV420P, 576u, 324u};
    return expect_scores(&c, TEMPORAL_FEATURE);
}

/* speed_temporal: boundary */

static char *test_temporal_prescale_1_0_default_path_extracts_three_frames(void)
{
    const FbCase c = {"speed_temporal", "1.0", NULL, VMAF_PIX_FMT_YUV420P, 576u, 324u};
    return expect_scores(&c, TEMPORAL_FEATURE);
}

static char *test_temporal_prescale_4_0_option_maximum_extracts_three_frames(void)
{
    const FbCase c = {"speed_temporal", "4.0", NULL, VMAF_PIX_FMT_YUV420P, 576u, 324u};
    return expect_scores(&c, TEMPORAL_FEATURE);
}

/* speed_temporal: negative */

static char *test_temporal_prescale_above_option_maximum_is_refused(void)
{
    const FbCase c = {"speed_temporal", "4.5", NULL, VMAF_PIX_FMT_YUV420P, 576u, 324u};
    VmafFeatureExtractorContext *ctx = NULL;
    const int err = create_context(&c, &ctx);
    if (!err) {
        const int err_destroy = vmaf_feature_extractor_context_destroy(ctx);
        mu_assert("extractor context destroy failed", err_destroy == 0);
        return "speed_prescale=4.5 was accepted above the 4.0 option maximum";
    }
    mu_assert("a refused context must not be returned", ctx == NULL);
    return NULL;
}

/* speed_chroma: positive */

static char *test_chroma_420_odd_width_and_height(void)
{
    const FbCase c = {"speed_chroma", NULL, NULL, VMAF_PIX_FMT_YUV420P, 577u, 325u};
    return expect_scores(&c, CHROMA_FEATURE);
}

static char *test_chroma_420_odd_width(void)
{
    const FbCase c = {"speed_chroma", NULL, NULL, VMAF_PIX_FMT_YUV420P, 577u, 324u};
    return expect_scores(&c, CHROMA_FEATURE);
}

static char *test_chroma_420_odd_height(void)
{
    const FbCase c = {"speed_chroma", NULL, NULL, VMAF_PIX_FMT_YUV420P, 576u, 325u};
    return expect_scores(&c, CHROMA_FEATURE);
}

static char *test_chroma_422_odd_width(void)
{
    const FbCase c = {"speed_chroma", NULL, NULL, VMAF_PIX_FMT_YUV422P, 577u, 162u};
    return expect_scores(&c, CHROMA_FEATURE);
}

/* speed_chroma: boundary */

static char *test_chroma_420_even_smallest_plane(void)
{
    const FbCase c = {"speed_chroma", NULL, NULL, VMAF_PIX_FMT_YUV420P, 160u, 160u};
    return expect_scores(&c, CHROMA_FEATURE);
}

static char *test_chroma_420_odd_smallest_plane(void)
{
    const FbCase c = {"speed_chroma", NULL, NULL, VMAF_PIX_FMT_YUV420P, 159u, 159u};
    return expect_scores(&c, CHROMA_FEATURE);
}

/* speed_chroma: negative */

static char *test_chroma_420_plane_below_one_block_is_refused(void)
{
    const FbCase c = {"speed_chroma", NULL, NULL, VMAF_PIX_FMT_YUV420P, 157u, 157u};
    int extract_err = 0;
    mu_assert_msg(run_case(&c, CHROMA_FEATURE, &extract_err, NULL));
    mu_assert("a 79x79 chroma plane was accepted", extract_err != 0);
    return NULL;
}

/* prescale within the identity tolerance that still changes the plane size */

static char *test_temporal_near_one_prescale_resamples_a_growing_plane(void)
{
    /* 576 * 1.0009 and 576 * 1.0015 both round to 577; 160 stays 160. */
    const FbCase inside = {"speed_temporal", "1.0009", NULL, VMAF_PIX_FMT_YUV420P, 576u, 160u};
    const FbCase outside = {"speed_temporal", "1.0015", NULL, VMAF_PIX_FMT_YUV420P, 576u, 160u};
    return expect_same_scores(&inside, &outside, TEMPORAL_FEATURE);
}

static char *test_chroma_near_one_prescale_resamples_a_shrinking_plane(void)
{
    /* 577 * 0.9991 and 577 * 0.998 both round to 576; 160 stays 160. Nearest
     * resampling by one column keeps columns 0..575, the same as a crop, so
     * the case uses bilinear, which interpolates. */
    const FbCase inside = {"speed_chroma", "0.9991", "bilinear", VMAF_PIX_FMT_YUV444P, 577u, 160u};
    const FbCase outside = {"speed_chroma", "0.998", "bilinear", VMAF_PIX_FMT_YUV444P, 577u, 160u};
    mu_assert_msg(expect_same_scores(&inside, &outside, CHROMA_U_FEATURE));
    mu_assert_msg(expect_same_scores(&inside, &outside, CHROMA_V_FEATURE));
    return expect_same_scores(&inside, &outside, CHROMA_FEATURE);
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_temporal_prescale_1_5_extracts_three_frames),
        MU_TEST(test_temporal_prescale_2_0_extracts_three_frames),
        MU_TEST(test_temporal_prescale_1_0_default_path_extracts_three_frames),
        MU_TEST(test_temporal_prescale_4_0_option_maximum_extracts_three_frames),
        MU_TEST(test_temporal_prescale_above_option_maximum_is_refused),
        MU_TEST(test_chroma_420_odd_width_and_height),
        MU_TEST(test_chroma_420_odd_width),
        MU_TEST(test_chroma_420_odd_height),
        MU_TEST(test_chroma_422_odd_width),
        MU_TEST(test_chroma_420_even_smallest_plane),
        MU_TEST(test_chroma_420_odd_smallest_plane),
        MU_TEST(test_chroma_420_plane_below_one_block_is_refused),
        MU_TEST(test_temporal_near_one_prescale_resamples_a_growing_plane),
        MU_TEST(test_chroma_near_one_prescale_resamples_a_shrinking_plane),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
