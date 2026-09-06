/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Unit and end-to-end tests for percentile pooling methods (Netflix#818, ADR-1181).
 *  Pins mathematical correctness of linear-interpolated percentiles,
 *  agreement with Python runner golden oracles on src01_hrc00/01 576x324 YUV,
 *  and error handling.
 */

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"
#include "libvmaf/picture.h"
#include "pooling_percentile.h"

#ifndef TESTDATA_YUV_DIR
#define TESTDATA_YUV_DIR "."
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit where NULL is the canonical null pointer constant, and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

static char *test_percentile_math_unit(void)
{
    const double vals[5] = {10.0, 20.0, 30.0, 40.0, 50.0};
    assert(sizeof(vals) / sizeof(vals[0]) == 5);

    /* 0th percentile is minimum */
    const double p0 = percentile(vals, 5, 0.0);
    mu_assert("percentile 0% mismatch", fabs(p0 - 10.0) < 1e-9);

    /* 100th percentile is maximum */
    const double p100 = percentile(vals, 5, 100.0);
    mu_assert("percentile 100% mismatch", fabs(p100 - 50.0) < 1e-9);

    /* 50th percentile is median */
    const double p50 = percentile(vals, 5, 50.0);
    mu_assert("percentile 50% mismatch", fabs(p50 - 30.0) < 1e-9);

    /* 25th percentile: idx = 0.25 * 4 = 1.0 -> 20.0 */
    const double p25 = percentile(vals, 5, 25.0);
    mu_assert("percentile 25% mismatch", fabs(p25 - 20.0) < 1e-9);

    /* 12.5th percentile: idx = 0.125 * 4 = 0.5 -> 0.5 * 10 + 0.5 * 20 = 15.0 */
    const double p12_5 = percentile(vals, 5, 12.5);
    mu_assert("percentile 12.5% mismatch", fabs(p12_5 - 15.0) < 1e-9);

    return NULL;
}

static int load_frame(VmafPicture *pic, FILE *f, unsigned w, unsigned h)
{
    assert(pic != NULL);
    assert(f != NULL);
    assert(w > 0 && h > 0);

    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, w, h);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned r = 0; r < h; r++) {
        if (fread(y + (size_t)r * (size_t)pic->stride[0], 1, w, f) != w) {
            vmaf_picture_unref(pic);
            return -1;
        }
    }
    uint8_t *u = (uint8_t *)pic->data[1];
    for (unsigned r = 0; r < h / 2; r++) {
        if (fread(u + (size_t)r * (size_t)pic->stride[1], 1, w / 2, f) != w / 2) {
            vmaf_picture_unref(pic);
            return -1;
        }
    }
    uint8_t *v = (uint8_t *)pic->data[2];
    for (unsigned r = 0; r < h / 2; r++) {
        if (fread(v + (size_t)r * (size_t)pic->stride[2], 1, w / 2, f) != w / 2) {
            vmaf_picture_unref(pic);
            return -1;
        }
    }
    return 0;
}

static int feed_one_frame(VmafContext *vmaf, FILE *f_ref, FILE *f_dis, unsigned w, unsigned h,
                          unsigned index)
{
    VmafPicture ref;
    VmafPicture dis;
    int err = load_frame(&ref, f_ref, w, h);
    if (err) {
        return err;
    }
    err = load_frame(&dis, f_dis, w, h);
    if (err) {
        vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dis, index);
}

/* Feed the Netflix 576x324 reference pair through @p vmaf and flush EOS. Both
 * YUV streams are closed on every exit path before the first assertion that can
 * return, so a mid-loop failure cannot leak them (clang-analyzer-unix.Stream). */
static char *feed_reference_pair(VmafContext *vmaf, unsigned w, unsigned h, unsigned n_frames)
{
    FILE *f_ref = fopen(TESTDATA_YUV_DIR "/src01_hrc00_576x324.yuv", "rb");
    mu_assert("could not open reference YUV", f_ref != NULL);
    FILE *f_dis = fopen(TESTDATA_YUV_DIR "/src01_hrc01_576x324.yuv", "rb");
    if (!f_dis) {
        (void)fclose(f_ref);
    }
    mu_assert("could not open distorted YUV", f_dis != NULL);

    int err = 0;
    for (unsigned i = 0; (i < n_frames) && (err == 0); i++) {
        err = feed_one_frame(vmaf, f_ref, f_dis, w, h, i);
    }
    (void)fclose(f_ref);
    (void)fclose(f_dis);
    mu_assert("reading the reference pair failed", err == 0);

    /* Flush EOS */
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("vmaf_read_pictures(EOS) failed", err == 0);
    return NULL;
}

static char *check_percentile_oracles(VmafContext *vmaf, VmafModel *model, unsigned n_frames,
                                      double *p5, double *p10, double *p20)
{
    /* PERC10 oracle: matches quality_runner_test.py:680 within 1e-2 */
    int err = vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_PERC10, p10, 0, n_frames - 1);
    mu_assert("score_pooled PERC10 failed", err == 0);
    mu_assert("score_pooled PERC10 near python oracle", fabs(*p10 - 72.71845922683059) < 1e-2);
    mu_assert("score_pooled PERC10 exact reference", fabs(*p10 - 72.71734120) < 1e-4);

    /* PERC5 oracle: 72.35185185 */
    err = vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_PERC5, p5, 0, n_frames - 1);
    mu_assert("score_pooled PERC5 failed", err == 0);
    mu_assert("score_pooled PERC5 exact reference", fabs(*p5 - 72.35185185) < 1e-4);

    /* PERC20 oracle: 73.35746240 */
    err = vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_PERC20, p20, 0, n_frames - 1);
    mu_assert("score_pooled PERC20 failed", err == 0);
    mu_assert("score_pooled PERC20 exact reference", fabs(*p20 - 73.35746240) < 1e-4);
    return NULL;
}

static char *check_central_oracles(VmafContext *vmaf, VmafModel *model, unsigned n_frames,
                                   double p5, double p10, double p20)
{
    /* MEDIAN oracle: 76.09165850 */
    double score_med = 0.0;
    int err = vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_MEDIAN, &score_med, 0, n_frames - 1);
    mu_assert("score_pooled MEDIAN failed", err == 0);
    mu_assert("score_pooled MEDIAN exact reference", fabs(score_med - 76.09165850) < 1e-4);

    /* MEAN oracle: 76.66782963 — the pre-existing pooling path must not move. */
    double score_mean = 0.0;
    err = vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_MEAN, &score_mean, 0, n_frames - 1);
    mu_assert("score_pooled MEAN failed", err == 0);
    mu_assert("score_pooled MEAN exact reference", fabs(score_mean - 76.66782963) < 1e-4);

    /* Monotonicity sanity check across percentiles */
    mu_assert("percentile monotonicity p5 <= p10", p5 <= p10);
    mu_assert("percentile monotonicity p10 <= p20", p10 <= p20);
    mu_assert("percentile monotonicity p20 <= median", p20 <= score_med);
    return NULL;
}

static char *check_pooled_error_paths(VmafContext *vmaf, VmafModel *model, unsigned n_frames)
{
    double score = 0.0;
    int err = vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_MEDIAN, NULL, 0, n_frames - 1);
    mu_assert("score_pooled with NULL score must return -EINVAL", err == -EINVAL);

    err = vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_MEDIAN, &score, 10, 5);
    mu_assert("score_pooled with inverted index must return -EINVAL", err == -EINVAL);
    return NULL;
}

static char *test_pooling_percentile_yuv(void)
{
    const unsigned w = 576;
    const unsigned h = 324;
    const unsigned n_frames = 48;
    assert(w > 0 && h > 0 && n_frames > 0);

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("vmaf_init failed", err == 0);

    VmafModelConfig mcfg = {0};
    VmafModel *model = NULL;
    err = vmaf_model_load(&model, &mcfg, "vmaf_v0.6.1");
    mu_assert("vmaf_model_load failed", err == 0);
    err = vmaf_use_features_from_model(vmaf, model);
    mu_assert("vmaf_use_features_from_model failed", err == 0);

    char *msg = feed_reference_pair(vmaf, w, h, n_frames);
    if (msg) {
        return msg;
    }

    double p5 = 0.0;
    double p10 = 0.0;
    double p20 = 0.0;
    msg = check_percentile_oracles(vmaf, model, n_frames, &p5, &p10, &p20);
    if (msg) {
        return msg;
    }
    msg = check_central_oracles(vmaf, model, n_frames, p5, p10, p20);
    if (msg) {
        return msg;
    }
    msg = check_pooled_error_paths(vmaf, model, n_frames);
    if (msg) {
        return msg;
    }

    err = vmaf_close(vmaf);
    mu_assert("vmaf_close failed", err == 0);
    vmaf_model_destroy(model);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_percentile_math_unit);
    mu_run_test(test_pooling_percentile_yuv);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
