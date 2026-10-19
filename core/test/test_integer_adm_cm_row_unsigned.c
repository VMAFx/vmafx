/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * T-ADM-CM-SCALE0-ROW-INT64-OVERFLOW-2026-10-05: the integer ADM extractor
 * summed each scale-0 contrast-masking row in int64. The terms are
 * non-negative cubes, and a row of them can pass INT64_MAX: on the picture of
 * adm_cm_row_overflow_frame.h (64x64, compared with itself) the row reaches
 * 1.021 INT64_MAX at 16 bits. The signed sum wrapped (undefined behaviour),
 * the numerator came out NaN and the frame failed with "invalid ADM
 * reduction". The rows and the frame are summed unsigned now
 * (adm_cm_round_row_total_s0()).
 *
 * Per bit depth (8, 10, 16) this test scores the picture with the host's
 * SIMD dispatch and with every SIMD flag off, and holds:
 *   - the frame scores (it failed before the change);
 *   - integer_adm_scale0 and the adm2 score are within 1e-3 of 1: the picture is
 *     compared with itself, and float_adm gives 1;
 *   - the two dispatch paths give the same bits.
 * The GPU twins are held to the scalar CPU on the same picture by
 * test_gpu_adm_tiny_frames.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "adm_cm_row_overflow_frame.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

#define CPUMASK_HOST 0u
#define CPUMASK_SCALAR UINT64_MAX
#define NUM_SCORES 2u
#define REFERENCE_TOL 1e-3

static const char *const SCORE_KEYS[NUM_SCORES] = {"integer_adm_scale0",
                                                   "VMAF_integer_feature_adm2_score"};

/* Luma sample (row, col) of `pic`; vmaf_picture_alloc() zero-fills the rest. */
static void put_luma(VmafPicture *pic, unsigned row, unsigned col, uint16_t v)
{
    if (pic->bpc == 8u) {
        ((uint8_t *)pic->data[0])[(row * pic->stride[0]) + col] = (uint8_t)v;
    } else {
        ((uint16_t *)pic->data[0])[(row * (pic->stride[0] / 2)) + col] = v;
    }
}

static int fill_picture(VmafPicture *pic, unsigned bpc)
{
    const int err =
        vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV444P, bpc, ADM_ROW_OVERFLOW_W, ADM_ROW_OVERFLOW_H);
    if (err) {
        return err;
    }
    const uint16_t max = (uint16_t)((1u << bpc) - 1u);
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            put_luma(pic, row, col, adm_row_overflow_sample(row, col, max));
        }
    }
    return 0;
}

static int finish(VmafContext *vmaf, int err)
{
    const int close_err = vmaf_close(vmaf);
    return err ? err : close_err;
}

/* One frame of the picture against itself; the two scores of SCORE_KEYS. */
static int score(unsigned bpc, uint64_t cpumask, double out[NUM_SCORES])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .cpumask = cpumask};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (err) {
        return err;
    }
    err = vmaf_use_feature(vmaf, "adm", NULL);
    if (err) {
        return finish(vmaf, err);
    }
    VmafPicture ref;
    VmafPicture dist;
    err = fill_picture(&ref, bpc);
    if (err) {
        return finish(vmaf, err);
    }
    err = fill_picture(&dist, bpc);
    if (err) {
        const int unref_err = vmaf_picture_unref(&ref);
        return finish(vmaf, err ? err : unref_err);
    }
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    if (!err) {
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0u);
    }
    for (unsigned k = 0; !err && k < NUM_SCORES; k++) {
        err = vmaf_feature_score_at_index(vmaf, SCORE_KEYS[k], &out[k], 0u);
    }
    return finish(vmaf, err);
}

static uint64_t score_bits(double x)
{
    uint64_t bits = 0;
    memcpy(&bits, &x, sizeof(bits));
    return bits;
}

static char *check_depth(unsigned bpc)
{
    double host[NUM_SCORES] = {0};
    double scalar[NUM_SCORES] = {0};
    mu_assert("integer adm fails the frame (host dispatch)", !score(bpc, CPUMASK_HOST, host));
    mu_assert("integer adm fails the frame (scalar)", !score(bpc, CPUMASK_SCALAR, scalar));
    for (unsigned k = 0; k < NUM_SCORES; k++) {
        mu_assert("score is not finite", isfinite(host[k]));
        mu_assert("score is not 1 for a picture compared with itself",
                  fabs(host[k] - 1.0) < REFERENCE_TOL);
        mu_assert("host dispatch and scalar code differ",
                  score_bits(host[k]) == score_bits(scalar[k]));
    }
    return NULL;
}

static char *test_row_past_int64_max_scores_at_16_bits(void)
{
    return check_depth(16u);
}

static char *test_row_past_int64_max_scores_at_10_bits(void)
{
    return check_depth(10u);
}

static char *test_row_past_int64_max_scores_at_8_bits(void)
{
    return check_depth(8u);
}

char *run_tests(void)
{
    mu_run_test(test_row_past_int64_max_scores_at_16_bits);
    mu_run_test(test_row_past_int64_max_scores_at_10_bits);
    mu_run_test(test_row_past_int64_max_scores_at_8_bits);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
