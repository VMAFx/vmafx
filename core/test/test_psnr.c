/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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

#include "test.h"
// NOLINTNEXTLINE(bugprone-suspicious-include): white-box seam (ADR-0141).
#include "feature/integer_psnr.c"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr`. This
 * test follows the cross-platform spelling of the surface it exercises.
 * ADR-1138. */

#define EPS 0.00001

static int almost_equal(double a, double b)
{
    double diff = a > b ? a - b : b - a;
    return diff < EPS;
}

static int get_picture_16b(VmafPicture *pic, int pic_index)
{
    int count = 0;
    uint16_t sample_pic[2][16] = {
        {0, 0, 0, 0, 0, 0},
        {65535, 65535, 65535, 65535, 65535, 65535},
    };

    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 16, 2, 2);
    if (err)
        return err;
    for (int c = 0; c < 3; c++) {
        uint16_t *data = (uint16_t *)pic->data[c];
        int stride = pic->stride[c] >> 1;
        for (unsigned i = 0; i < pic->h[c]; i++) {
            for (unsigned j = 0; j < pic->w[c]; j++) {
                data[i * stride + j] = sample_pic[pic_index][count++];
            }
        }
    }
    return 0;
}

static char *check_scores(VmafFeatureCollector *fc)
{
    double psnr_y = 0.0;
    double psnr_cb = 0.0;
    double psnr_cr = 0.0;
    int err = 0;
    err |= vmaf_feature_collector_get_score(fc, "psnr_y", &psnr_y, 0);
    err |= vmaf_feature_collector_get_score(fc, "psnr_cb", &psnr_cb, 0);
    err |= vmaf_feature_collector_get_score(fc, "psnr_cr", &psnr_cr, 0);
    double mse_y = 0.0;
    double mse_cb = 0.0;
    double mse_cr = 0.0;
    err |= vmaf_feature_collector_get_score(fc, "mse_y", &mse_y, 0);
    err |= vmaf_feature_collector_get_score(fc, "mse_cb", &mse_cb, 0);
    err |= vmaf_feature_collector_get_score(fc, "mse_cr", &mse_cr, 0);
    mu_assert("test_16b_large_diff vmaf_feature_collector_get_score error", !err);

    mu_assert("wrong psnr_y", almost_equal(psnr_y, 0.0));
    mu_assert("wrong psnr_cb", almost_equal(psnr_cb, 0.0));
    mu_assert("wrong psnr_cr", almost_equal(psnr_cr, 0.0));
    mu_assert("wrong mse_y", almost_equal(mse_y, 4294836225.0));
    mu_assert("wrong mse_cb", almost_equal(mse_cb, 4294836225.0));
    mu_assert("wrong mse_cr", almost_equal(mse_cr, 4294836225.0));
    return NULL;
}

static char *test_16b_large_diff(void)
{
    VmafPicture pic1;
    VmafPicture pic2;
    int err = 0;
    err |= get_picture_16b(&pic1, 0);
    err |= get_picture_16b(&pic2, 1);
    mu_assert("test_16b_large_diff alloc error", !err);
    VmafFeatureCollector *fc;
    err |= vmaf_feature_collector_init(&fc);
    mu_assert("test_16b_large_diff vmaf_feature_collector_init error", !err);
    PsnrState psnr_state = {
        .enable_chroma = 1,
        .enable_mse = 1,
        .enable_apsnr = 0,
        .peak = 65535,
    };

    err |= psnr_hbd(&pic1, &pic2, 0, fc, &psnr_state);
    mu_assert("failed psnr_hbd", err == 0);

    char *msg = check_scores(fc);

    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&pic1);
    vmaf_picture_unref(&pic2);

    return msg;
}

/* Every depth from 9 to 15 bits scores: a difference of one code on every sample is an MSE of 1
 * and a PSNR of 20 log10(2^bpc - 1), at 9, 11, 13, 14 and 15 bits (9 and 14 were refused). */
static char *test_odd_depths(void)
{
    static const unsigned depths[] = {9u, 11u, 13u, 14u, 15u};
    for (size_t k = 0; k < sizeof(depths) / sizeof(depths[0]); k++) {
        const unsigned bpc = depths[k];
        VmafPicture ref;
        VmafPicture dist;
        mu_assert("alloc ref", vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV420P, bpc, 4, 4) == 0);
        mu_assert("alloc dist", vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV420P, bpc, 4, 4) == 0);
        for (int c = 0; c < 3; c++) {
            for (unsigned i = 0; i < ref.h[c]; i++) {
                uint16_t *const r =
                    (uint16_t *)((uint8_t *)ref.data[c] + (size_t)i * ref.stride[c]);
                uint16_t *const d =
                    (uint16_t *)((uint8_t *)dist.data[c] + (size_t)i * dist.stride[c]);
                for (unsigned j = 0; j < ref.w[c]; j++) {
                    r[j] = (uint16_t)(100u + j);
                    d[j] = (uint16_t)(101u + j);
                }
            }
        }
        VmafFeatureCollector *fc = NULL;
        mu_assert("collector", vmaf_feature_collector_init(&fc) == 0);
        PsnrState state = {.enable_chroma = 1, .peak = (1u << bpc) - 1u, .uncapped = true};
        VmafFeatureExtractor fex = {.priv = &state};
        const int err = extract(&fex, &ref, NULL, &dist, NULL, 0, fc);
        double psnr_y = 0.0;
        const int got = vmaf_feature_collector_get_score(fc, "psnr_y", &psnr_y, 0);
        const double want = 20.0 * log10((double)((1u << bpc) - 1u));
        vmaf_feature_collector_destroy(fc);
        vmaf_picture_unref(&ref);
        vmaf_picture_unref(&dist);
        mu_assert("the extractor scores this depth", err == 0 && got == 0);
        mu_assert("psnr of a one-code difference", almost_equal(psnr_y, want));
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_16b_large_diff);
    mu_run_test(test_odd_depths);

    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
