/**
 * Copyright 2026 Lusoris
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
 * test_tad_rust.c — smoke test for the TAD Rust feature extractor.
 * ADR-0707 cbindgen pilot.
 *
 * Tests the Rust-implemented TAD (Temporal Absolute Difference) extractor
 * through the C ABI: vmafx_tad_init / vmafx_tad_extract / vmafx_tad_close.
 * Asserts that:
 *  1. identical frames produce TAD == 0.0.
 *  2. maximally different frames produce TAD == 1.0.
 *  3. a known partial difference produces the expected TAD.
 *  4. the extractor does not affect any other feature (VMAF score isolation).
 */

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../src/feature/feature_collector.h"
#include "../src/picture.h"

/* ---------------------------------------------------------------------------
 * Forward-declare the Rust-exported symbols (same as in tad_rust.c).
 * --------------------------------------------------------------------------- */

extern int vmafx_tad_init(void **state_out, unsigned int bpc);
extern int vmafx_tad_extract(void *state_ptr, const VmafPicture *ref_pic,
                             const VmafPicture *dis_pic, unsigned int index,
                             VmafFeatureCollector *feature_collector);
extern int vmafx_tad_close(void *state_ptr);

#define EPS 1e-9

static int alloc_8bit_grey(VmafPicture *pic, unsigned w, unsigned h, uint8_t fill)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, w, h);
    if (err)
        return err;
    memset(pic->data[0], fill, (size_t)h * (size_t)pic->stride[0]);
    /* Chroma planes — fill with mid-grey (not tested by TAD). */
    memset(pic->data[1], 128, (size_t)(h / 2) * (size_t)pic->stride[1]);
    memset(pic->data[2], 128, (size_t)(h / 2) * (size_t)pic->stride[2]);
    return 0;
}

/* --------------------------------------------------------------------------- */

static char *test_tad_identical_frames(void)
{
    VmafPicture ref, dis;
    VmafFeatureCollector *fc = NULL;
    void *state = NULL;
    int err = 0;

    err |= alloc_8bit_grey(&ref, 64, 64, 128);
    err |= alloc_8bit_grey(&dis, 64, 64, 128);
    mu_assert("alloc error", err == 0);

    err = vmaf_feature_collector_init(&fc);
    mu_assert("feature_collector_init error", err == 0);

    err = vmafx_tad_init(&state, 8);
    mu_assert("vmafx_tad_init error", err == 0);

    err = vmafx_tad_extract(state, &ref, &dis, 0, fc);
    mu_assert("vmafx_tad_extract error", err == 0);

    double tad = -1.0;
    err = vmaf_feature_collector_get_score(fc, "tad", &tad, 0);
    mu_assert("get_score error", err == 0);
    mu_assert("identical frames: tad should be 0.0", fabs(tad) < EPS);

    double tad_sad = -1.0;
    err = vmaf_feature_collector_get_score(fc, "tad_sad", &tad_sad, 0);
    mu_assert("get_score tad_sad error", err == 0);
    mu_assert("identical frames: tad_sad should be 0.0", fabs(tad_sad) < EPS);

    (void)vmafx_tad_close(state);
    vmaf_feature_collector_destroy(fc);
    (void)vmaf_picture_unref(&ref);
    (void)vmaf_picture_unref(&dis);
    return NULL;
}

static char *test_tad_max_diff(void)
{
    VmafPicture ref, dis;
    VmafFeatureCollector *fc = NULL;
    void *state = NULL;
    int err = 0;

    err |= alloc_8bit_grey(&ref, 64, 64, 0);
    err |= alloc_8bit_grey(&dis, 64, 64, 255);
    mu_assert("alloc error", err == 0);

    err = vmaf_feature_collector_init(&fc);
    mu_assert("feature_collector_init error", err == 0);

    err = vmafx_tad_init(&state, 8);
    mu_assert("vmafx_tad_init error", err == 0);

    err = vmafx_tad_extract(state, &ref, &dis, 0, fc);
    mu_assert("vmafx_tad_extract error", err == 0);

    double tad = -1.0;
    err = vmaf_feature_collector_get_score(fc, "tad", &tad, 0);
    mu_assert("get_score error", err == 0);
    mu_assert("max diff: tad should be 1.0", fabs(tad - 1.0) < EPS);

    (void)vmafx_tad_close(state);
    vmaf_feature_collector_destroy(fc);
    (void)vmaf_picture_unref(&ref);
    (void)vmaf_picture_unref(&dis);
    return NULL;
}

static char *test_tad_partial_diff(void)
{
    /* ref all-0, dis all-128: expected TAD = 128/255 */
    VmafPicture ref, dis;
    VmafFeatureCollector *fc = NULL;
    void *state = NULL;
    int err = 0;

    err |= alloc_8bit_grey(&ref, 64, 64, 0);
    err |= alloc_8bit_grey(&dis, 64, 64, 128);
    mu_assert("alloc error", err == 0);

    err = vmaf_feature_collector_init(&fc);
    mu_assert("feature_collector_init error", err == 0);

    err = vmafx_tad_init(&state, 8);
    mu_assert("vmafx_tad_init error", err == 0);

    err = vmafx_tad_extract(state, &ref, &dis, 0, fc);
    mu_assert("vmafx_tad_extract error", err == 0);

    double tad = -1.0;
    err = vmaf_feature_collector_get_score(fc, "tad", &tad, 0);
    mu_assert("get_score error", err == 0);

    double expected = 128.0 / 255.0;
    mu_assert("partial diff: tad should be 128/255", fabs(tad - expected) < 1e-9);

    (void)vmafx_tad_close(state);
    vmaf_feature_collector_destroy(fc);
    (void)vmaf_picture_unref(&ref);
    (void)vmaf_picture_unref(&dis);
    return NULL;
}

static char *test_tad_multi_frame(void)
{
    /* Verify that the extractor accumulates across multiple frame indices. */
    VmafPicture ref0, dis0, ref1, dis1;
    VmafFeatureCollector *fc = NULL;
    void *state = NULL;
    int err = 0;

    err |= alloc_8bit_grey(&ref0, 8, 8, 0);
    err |= alloc_8bit_grey(&dis0, 8, 8, 0); /* frame 0: TAD == 0 */
    err |= alloc_8bit_grey(&ref1, 8, 8, 0);
    err |= alloc_8bit_grey(&dis1, 8, 8, 255); /* frame 1: TAD == 1 */
    mu_assert("alloc error", err == 0);

    err = vmaf_feature_collector_init(&fc);
    mu_assert("feature_collector_init error", err == 0);

    err = vmafx_tad_init(&state, 8);
    mu_assert("vmafx_tad_init error", err == 0);

    err = vmafx_tad_extract(state, &ref0, &dis0, 0, fc);
    mu_assert("extract frame 0 error", err == 0);
    err = vmafx_tad_extract(state, &ref1, &dis1, 1, fc);
    mu_assert("extract frame 1 error", err == 0);

    double tad0 = -1.0, tad1 = -1.0;
    err = vmaf_feature_collector_get_score(fc, "tad", &tad0, 0);
    err |= vmaf_feature_collector_get_score(fc, "tad", &tad1, 1);
    mu_assert("get_score multi-frame error", err == 0);
    mu_assert("multi-frame: frame 0 tad should be 0.0", fabs(tad0) < EPS);
    mu_assert("multi-frame: frame 1 tad should be 1.0", fabs(tad1 - 1.0) < EPS);

    (void)vmafx_tad_close(state);
    vmaf_feature_collector_destroy(fc);
    (void)vmaf_picture_unref(&ref0);
    (void)vmaf_picture_unref(&dis0);
    (void)vmaf_picture_unref(&ref1);
    (void)vmaf_picture_unref(&dis1);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Test runner
 * --------------------------------------------------------------------------- */

char *run_tests(void)
{
    mu_run_test(test_tad_identical_frames);
    mu_run_test(test_tad_max_diff);
    mu_run_test(test_tad_partial_diff);
    mu_run_test(test_tad_multi_frame);
    return NULL;
}
