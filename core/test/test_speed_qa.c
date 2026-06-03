/**
 *
 *  Copyright 2026 Lusoris
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

/*
 * SpEED-QA extractor smoke tests -- ADR-0253.
 *
 * 1. Registration: discoverable by name and by provided-feature-name.
 * 2. VTable: init/extract/close non-NULL; priv_size non-zero.
 * 3. Flat-grey input (frame 0): score is finite and not NaN.
 * 4. Noise-textured (checkerboard) dist: entropy exceeds flat-grey entropy.
 * 5. Two-frame 0->255 step: frame-1 score (spatial+temporal) > frame-0 score.
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "libvmaf/picture.h"

#define SQA_W (64u)
#define SQA_H (64u)

static int alloc_grey_pic(VmafPicture *pic, uint8_t value)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV400P, 8, SQA_W, SQA_H);
    if (err)
        return err;
    uint8_t *p = pic->data[0];
    ptrdiff_t s = pic->stride[0];
    for (unsigned r = 0; r < SQA_H; r++)
        memset(p + r * s, value, SQA_W);
    return 0;
}

/* 2-pixel-period checkerboard: high local variance within each 7x7 block. */
static int alloc_noise_pic(VmafPicture *pic)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV400P, 8, SQA_W, SQA_H);
    if (err)
        return err;
    uint8_t *p = pic->data[0];
    ptrdiff_t s = pic->stride[0];
    for (unsigned r = 0; r < SQA_H; r++) {
        for (unsigned c = 0; c < SQA_W; c++) {
            unsigned phase = ((r / 2u) + (c / 2u)) & 1u;
            p[r * s + c] = phase ? 200u : 50u;
        }
    }
    return 0;
}

static char *test_speed_qa_is_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("speed_qa");
    mu_assert("speed_qa must be registered by name", fex != NULL);
    mu_assert("name must match", !strcmp(fex->name, "speed_qa"));
    mu_assert("init must be non-NULL", fex->init != NULL);
    mu_assert("extract must be non-NULL", fex->extract != NULL);
    mu_assert("close must be non-NULL", fex->close != NULL);
    mu_assert("priv_size must be non-zero", fex->priv_size > 0u);
    return NULL;
}

static char *test_speed_qa_provided_features_well_formed(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("speed_qa");
    mu_assert("speed_qa must resolve", fex != NULL);
    mu_assert("provided_features must be non-NULL", fex->provided_features != NULL);
    mu_assert("provided_features[0] must be non-NULL", fex->provided_features[0] != NULL);
    VmafFeatureExtractor *via_feat =
        vmaf_get_feature_extractor_by_feature_name(fex->provided_features[0], 0);
    mu_assert("feature-name lookup must round-trip", via_feat != NULL);
    mu_assert("round-trip resolves to speed_qa", !strcmp(via_feat->name, "speed_qa"));
    return NULL;
}

/* Run n_frames of the same ref/dist pair; return score at target_index. */
static char *run_n(VmafPicture *ref, VmafPicture *dist, unsigned n_frames, unsigned target_index,
                   double *out_score)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("speed_qa");
    mu_assert("speed_qa registered", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create ok", err == 0 && ctx != NULL);

    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV400P, 8, SQA_W, SQA_H);
    mu_assert("context_init ok", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init ok", err == 0 && fc != NULL);

    for (unsigned i = 0; i < n_frames; i++) {
        err = vmaf_feature_extractor_context_extract(ctx, ref, NULL, dist, NULL, i, fc);
        mu_assert("extract ok", err == 0);
    }

    err = vmaf_feature_collector_get_score(fc, "speed_qa", out_score, target_index);
    mu_assert("get_score ok", err == 0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    return NULL;
}

static char *test_speed_qa_flat_input_is_finite(void)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = alloc_grey_pic(&ref, 128);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey_pic(&dist, 128);
    mu_assert("alloc dist", err == 0);

    double score = 0.0;
    char *fail = run_n(&ref, &dist, 1, 0, &score);

    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    if (fail)
        return fail;

    mu_assert("flat score must be finite", isfinite(score));
    mu_assert("flat score must not be NaN", !isnan(score));
    return NULL;
}

static char *test_speed_qa_noise_higher_than_flat(void)
{
    VmafPicture ref_flat;
    VmafPicture dist_flat;
    VmafPicture ref_noise;
    VmafPicture dist_noise;

    int err = alloc_grey_pic(&ref_flat, 128);
    mu_assert("alloc flat ref", err == 0);
    err = alloc_grey_pic(&dist_flat, 128);
    mu_assert("alloc flat dist", err == 0);
    err = alloc_noise_pic(&ref_noise);
    mu_assert("alloc noise ref", err == 0);
    err = alloc_noise_pic(&dist_noise);
    mu_assert("alloc noise dist", err == 0);

    double score_flat = 0.0;
    double score_noise = 0.0;

    char *fail = run_n(&ref_flat, &dist_flat, 1, 0, &score_flat);
    if (fail) {
        vmaf_picture_unref(&ref_flat);
        vmaf_picture_unref(&dist_flat);
        vmaf_picture_unref(&ref_noise);
        vmaf_picture_unref(&dist_noise);
        return fail;
    }

    fail = run_n(&ref_noise, &dist_noise, 1, 0, &score_noise);
    vmaf_picture_unref(&ref_flat);
    vmaf_picture_unref(&dist_flat);
    vmaf_picture_unref(&ref_noise);
    vmaf_picture_unref(&dist_noise);
    if (fail)
        return fail;

    mu_assert("noise entropy must exceed flat entropy", score_noise > score_flat);
    return NULL;
}

static char *test_speed_qa_temporal_component_positive(void)
{
    VmafPicture ref0;
    VmafPicture dist0;
    VmafPicture ref1;
    VmafPicture dist1;

    int err = alloc_grey_pic(&ref0, 0);
    mu_assert("alloc ref0", err == 0);
    err = alloc_grey_pic(&dist0, 0);
    mu_assert("alloc dist0", err == 0);
    err = alloc_grey_pic(&ref1, 255);
    mu_assert("alloc ref1", err == 0);
    err = alloc_grey_pic(&dist1, 255);
    mu_assert("alloc dist1", err == 0);

    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("speed_qa");
    mu_assert("speed_qa registered", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create ok", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV400P, 8, SQA_W, SQA_H);
    mu_assert("context_init ok", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init ok", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref0, NULL, &dist0, NULL, 0, fc);
    mu_assert("extract frame 0 ok", err == 0);
    err = vmaf_feature_extractor_context_extract(ctx, &ref1, NULL, &dist1, NULL, 1, fc);
    mu_assert("extract frame 1 ok", err == 0);

    double score0 = 0.0;
    double score1 = 0.0;
    err = vmaf_feature_collector_get_score(fc, "speed_qa", &score0, 0);
    mu_assert("get score 0 ok", err == 0);
    err = vmaf_feature_collector_get_score(fc, "speed_qa", &score1, 1);
    mu_assert("get score 1 ok", err == 0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);

    vmaf_picture_unref(&ref0);
    vmaf_picture_unref(&dist0);
    vmaf_picture_unref(&ref1);
    vmaf_picture_unref(&dist1);

    /* Frame 1 has a maximum temporal diff (0->255 per pixel); its combined
     * score (spatial + temporal) must exceed the frame-0 spatial-only score. */
    mu_assert("frame-1 score > frame-0 (temporal component positive)", score1 > score0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_speed_qa_is_registered);
    mu_run_test(test_speed_qa_provided_features_well_formed);
    mu_run_test(test_speed_qa_flat_input_is_finite);
    mu_run_test(test_speed_qa_noise_higher_than_flat);
    mu_run_test(test_speed_qa_temporal_component_positive);
    return NULL;
}
