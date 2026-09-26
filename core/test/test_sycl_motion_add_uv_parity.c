/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-0989 / ADR-1326: motion_add_uv SYCL fixed-point parity test.
 *
 * Verifies two properties of the SYCL `motion_sycl` extractor when
 * `motion_add_uv=true` is set:
 *
 * 1. The SYCL score at frame index 1 differs from the Y-only score
 *    (i.e., the UV contribution is non-zero when the test fixture has
 *    non-uniform chroma motion).
 *
 * 2. The SYCL Y-only and Y+U+V scores match a scalar oracle that reproduces
 *    the fixed-point filter coefficients, per-pass rounding, reflect-101
 *    border handling, integer SAD accumulation, and per-plane normalization.
 *
 * Note: the CPU `motion` extractor (integer path) does NOT have
 * motion_add_uv. `float_motion` supports the same semantic option, but it is
 * not a numerical oracle for this fixed-point kernel: its float convolution
 * coefficients and float SAD reduction have a different rounding contract.
 *
 * Feature-name aliasing: when motion_add_uv=true (non-default), the
 * feature-name system appends "_mau" (the option alias) to the base
 * aliased name.  Scores must therefore be queried with the suffixed
 * name, not the raw VMAF_*_score name:
 *   float_motion  → "float_motion2_mau"   (VMAF_feature_motion2_score + _mau)
 *   motion_sycl   → "integer_motion2_mau" (VMAF_integer_feature_motion2_score + _mau)
 * See feature_name.c:vmaf_feature_name_from_opts_dict for the naming rule.
 *
 * Fixture: 256x144 YUV420P 8-bpc synthetic data where Y, U, and V
 * planes all contain frame-dependent ramps so chroma motion is
 * non-zero and the UV contribution is measurable.
 *
 * Skip behaviour: if vmaf_sycl_state_init() fails the test emits
 * "[skip: no SYCL device]" and passes cleanly.
 */

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "test.h"

#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Fixture geometry — large enough for the 5-tap Gaussian. */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#define FIXTURE_BPC 8u
#define NUM_FRAMES 2u

/* Fixed-point implementation constants from integer_motion_sycl.cpp. */
#define FIXED_BLUR_RADIUS 2
#define FIXED_BLUR_TAPS 5
#define FIXED_BLUR_SCALE 256.0
#define NORMALIZED_SCORE_ROUNDING_OPS 5.0

static const int32_t fixed_blur_filter[FIXED_BLUR_TAPS] = {3571, 16004, 26386, 16004, 3571};

/* Fill a YUV420P 8-bpc picture with a deterministic ramp on all three
 * planes.  Y, U, and V all vary with frame_idx so that consecutive
 * frames differ on all planes. */
static int fill_yuv_fixture(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    /* Luma ramp */
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + frame_idx * 13u) & 0xFFu);
        }
    }

    /* Chroma ramp — also varies with frame_idx so UV motion is non-zero */
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                plane[row * pic->stride[p] + col] =
                    (uint8_t)((row * 2u + col + frame_idx * 7u + p * 31u) & 0xFFu);
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Scalar fixed-point oracle — mirrors integer_motion_sycl.cpp.        */
/* ------------------------------------------------------------------ */
static int oracle_mirror(int idx, unsigned extent)
{
    if (idx < 0)
        return -idx;
    if ((unsigned)idx >= extent)
        return (int)(2u * extent) - idx - 2;
    return idx;
}

static uint8_t oracle_fixture_sample(unsigned plane, unsigned frame_idx, int row, int col,
                                     unsigned width, unsigned height)
{
    const unsigned y = (unsigned)oracle_mirror(row, height);
    const unsigned x = (unsigned)oracle_mirror(col, width);
    if (plane == 0u)
        return (uint8_t)((y + x + frame_idx * 13u) & 0xFFu);
    return (uint8_t)((y * 2u + x + frame_idx * 7u + plane * 31u) & 0xFFu);
}

static int32_t oracle_blur_pixel(unsigned plane, unsigned frame_idx, unsigned y, unsigned x,
                                 unsigned width, unsigned height)
{
    int32_t vertical[FIXED_BLUR_TAPS];
    const int32_t vertical_round = 1 << (FIXTURE_BPC - 1u);
    for (int hx = 0; hx < FIXED_BLUR_TAPS; hx++) {
        int32_t sum = 0;
        for (int hy = 0; hy < FIXED_BLUR_TAPS; hy++) {
            const int row = (int)y + hy - FIXED_BLUR_RADIUS;
            const int col = (int)x + hx - FIXED_BLUR_RADIUS;
            sum += fixed_blur_filter[hy] *
                   (int32_t)oracle_fixture_sample(plane, frame_idx, row, col, width, height);
        }
        vertical[hx] = (sum + vertical_round) >> FIXTURE_BPC;
    }

    int64_t horizontal = 0;
    for (int hx = 0; hx < FIXED_BLUR_TAPS; hx++)
        horizontal += (int64_t)fixed_blur_filter[hx] * vertical[hx];
    return (int32_t)((horizontal + (1 << 15)) >> 16);
}

static int64_t oracle_plane_sad(unsigned plane, unsigned width, unsigned height)
{
    int64_t sad = 0;
    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < width; x++) {
            const int32_t first = oracle_blur_pixel(plane, 0u, y, x, width, height);
            const int32_t second = oracle_blur_pixel(plane, 1u, y, x, width, height);
            const int64_t diff = (int64_t)second - first;
            sad += (diff < 0) ? -diff : diff;
        }
    }
    return sad;
}

static double oracle_normalize_sad(int64_t sad, unsigned width, unsigned height)
{
    return (double)sad / FIXED_BLUR_SCALE / ((double)width * height);
}

static void oracle_motion_scores(double *y_only, double *add_uv)
{
    const unsigned chroma_w = (FIXTURE_W + 1u) >> 1u;
    const unsigned chroma_h = (FIXTURE_H + 1u) >> 1u;
    *y_only =
        oracle_normalize_sad(oracle_plane_sad(0u, FIXTURE_W, FIXTURE_H), FIXTURE_W, FIXTURE_H);
    *add_uv = *y_only;
    *add_uv += oracle_normalize_sad(oracle_plane_sad(1u, chroma_w, chroma_h), chroma_w, chroma_h);
    *add_uv += oracle_normalize_sad(oracle_plane_sad(2u, chroma_w, chroma_h), chroma_w, chroma_h);
}

/* Each SAD is exactly representable for the admitted frame-size range. The
 * power-of-two /256 scaling is exact; only three area divisions and two sums
 * round. Comparing two independent evaluations doubles Higham's gamma_5. */
static double oracle_score_roundoff_bound(double expected)
{
    const double unit_roundoff = DBL_EPSILON / 2.0;
    const double gamma = (NORMALIZED_SCORE_ROUNDING_OPS * unit_roundoff) /
                         (1.0 - NORMALIZED_SCORE_ROUNDING_OPS * unit_roundoff);
    return 2.0 * gamma * fmax(1.0, fabs(expected));
}

/* ------------------------------------------------------------------ */
/* SYCL path — motion_sycl with motion_add_uv=true                    */
/* ------------------------------------------------------------------ */
/*
 * Pass 1: motion_sycl with motion_add_uv=true.
 *
 * `sycl_state` stays owned by run_sycl_motion_uv() below, which initialises it
 * once and releases it after both passes, so the two passes share one state
 * lifetime exactly as they did when they were two blocks of one function.
 */
static char *setup_sycl_pass_add_uv(VmafSyclState *sycl_state, VmafContext **out_vmaf)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("SYCL+UV: vmaf_init failed", !err);

    err = vmaf_sycl_import_state(vmaf, sycl_state);
    mu_assert("SYCL+UV: vmaf_sycl_import_state failed", !err);

    VmafFeatureDictionary *opts = NULL;
    err = vmaf_feature_dictionary_set(&opts, "motion_add_uv", "true");
    mu_assert("SYCL+UV: vmaf_feature_dictionary_set failed", !err);

    err = vmaf_use_feature(vmaf, "motion_sycl", opts);
    /* On success ownership transfers to vmaf — do not free. On failure free it. */
    if (err)
        (void)vmaf_feature_dictionary_free(&opts);
    mu_assert("SYCL+UV: vmaf_use_feature failed", !err);

    *out_vmaf = vmaf;
    return NULL;
}

static char *feed_sycl_pass_add_uv_frames(VmafContext *vmaf)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_yuv_fixture(&ref, i);
        mu_assert("SYCL+UV: fill_yuv_fixture(ref) failed", !err);
        err = fill_yuv_fixture(&dist, i);
        mu_assert("SYCL+UV: fill_yuv_fixture(dist) failed", !err);

        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("SYCL+UV: vmaf_read_pictures failed", !err);
    }

    int err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("SYCL+UV: EOS failed", !err);
    return NULL;
}

static char *run_sycl_pass_add_uv(VmafSyclState *sycl_state, double *out_score)
{
    VmafContext *vmaf = NULL;
    mu_assert_msg(setup_sycl_pass_add_uv(sycl_state, &vmaf));
    mu_assert_msg(feed_sycl_pass_add_uv_frames(vmaf));

    /* motion_sycl with motion_add_uv=true stores scores under the aliased
     * name: integer_motion2_mau (VMAF_integer_feature_motion2_score aliased
     * to integer_motion2, with _mau appended for the non-default bool
     * option).  See feature_name.c:vmaf_feature_name_from_opts_dict. */
    int err = vmaf_feature_score_at_index(vmaf, "integer_motion2_mau", out_score, 1u);
    mu_assert("SYCL+UV: vmaf_feature_score_at_index(integer_motion2_mau, idx=1) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("SYCL+UV: vmaf_close failed", !err);
    return NULL;
}

/* Pass 2: motion_sycl with default options — the Y-only baseline. Shares the
 * caller's `sycl_state` with pass 1. */
static char *setup_sycl_pass_y_only(VmafSyclState *sycl_state, VmafContext **out_vmaf)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf2 = NULL;
    int err = vmaf_init(&vmaf2, cfg);
    mu_assert("SYCL-Y: vmaf_init failed", !err);

    err = vmaf_sycl_import_state(vmaf2, sycl_state);
    mu_assert("SYCL-Y: vmaf_sycl_import_state failed", !err);

    err = vmaf_use_feature(vmaf2, "motion_sycl", NULL);
    mu_assert("SYCL-Y: vmaf_use_feature failed", !err);

    *out_vmaf = vmaf2;
    return NULL;
}

static char *feed_sycl_pass_y_only_frames(VmafContext *vmaf2)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_yuv_fixture(&ref, i);
        mu_assert("SYCL-Y: fill_yuv_fixture(ref) failed", !err);
        err = fill_yuv_fixture(&dist, i);
        mu_assert("SYCL-Y: fill_yuv_fixture(dist) failed", !err);

        err = vmaf_read_pictures(vmaf2, &ref, &dist, i);
        mu_assert("SYCL-Y: vmaf_read_pictures failed", !err);
    }

    int err = vmaf_read_pictures(vmaf2, NULL, NULL, 0);
    mu_assert("SYCL-Y: EOS failed", !err);
    return NULL;
}

static char *run_sycl_pass_y_only(VmafSyclState *sycl_state, double *out_score_y_only)
{
    VmafContext *vmaf2 = NULL;
    mu_assert_msg(setup_sycl_pass_y_only(sycl_state, &vmaf2));
    mu_assert_msg(feed_sycl_pass_y_only_frames(vmaf2));

    /* motion_sycl with default options (motion_add_uv=false) has no
     * non-default FEATURE_PARAM options, so the feature-name system uses
     * the raw name (no aliasing, no suffix). */
    int err = vmaf_feature_score_at_index(vmaf2, "VMAF_integer_feature_motion2_score",
                                          out_score_y_only, 1u);
    mu_assert("SYCL-Y: vmaf_feature_score_at_index(motion2, idx=1) failed", !err);

    err = vmaf_close(vmaf2);
    mu_assert("SYCL-Y: vmaf_close failed", !err);
    return NULL;
}

/* Run both passes over one SYCL state and report both scores. The state is
 * released on every exit path, including a failing pass. */
static char *run_sycl_motion_uv(double *out_score, double *out_score_y_only)
{
    *out_score = NAN;
    *out_score_y_only = NAN;

    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    const int err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err != 0 || sycl_state == NULL) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return NULL;
    }

    char *msg = run_sycl_pass_add_uv(sycl_state, out_score);
    if (!msg)
        msg = run_sycl_pass_y_only(sycl_state, out_score_y_only);

    vmaf_sycl_state_free(&sycl_state);
    return msg;
}

/* ------------------------------------------------------------------ */
/* Test 1: UV contribution is non-zero                                 */
/* ------------------------------------------------------------------ */
static char *test_motion_add_uv_increases_score(void)
{
    double sycl_uv = NAN;
    double sycl_y = NAN;

    char *msg = run_sycl_motion_uv(&sycl_uv, &sycl_y);
    if (msg)
        return msg;

    if (isnan(sycl_uv))
        return NULL; /* no SYCL device — skip */

    if (sycl_uv <= sycl_y) {
        (void)fprintf(stderr,
                      "\nmotion_add_uv FAIL: UV score (%.8f) <= Y-only score (%.8f); "
                      "UV contribution should be positive\n",
                      sycl_uv, sycl_y);
    }
    mu_assert("motion_add_uv score should exceed Y-only score (UV contribution must be > 0)",
              sycl_uv > sycl_y);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Test 2: SYCL fixed-point scores match the scalar fixed-point oracle. */
/* ------------------------------------------------------------------ */
static char *test_motion_add_uv_fixed_oracle_parity(void)
{
    double sycl_score = NAN;
    double sycl_y = NAN;
    char *msg = run_sycl_motion_uv(&sycl_score, &sycl_y);
    if (msg)
        return msg;

    if (isnan(sycl_score))
        return NULL; /* no SYCL device — skip */

    double oracle_y = 0.0;
    double oracle_uv = 0.0;
    oracle_motion_scores(&oracle_y, &oracle_uv);
    const double y_delta = fabs(sycl_y - oracle_y);
    const double uv_delta = fabs(sycl_score - oracle_uv);
    const double y_bound = oracle_score_roundoff_bound(oracle_y);
    const double uv_bound = oracle_score_roundoff_bound(oracle_uv);
    if (y_delta > y_bound || uv_delta > uv_bound) {
        (void)fprintf(stderr,
                      "\nmotion_add_uv fixed-oracle FAIL: "
                      "y(sycl=%.17g oracle=%.17g delta=%.3e bound=%.3e) "
                      "uv(sycl=%.17g oracle=%.17g delta=%.3e bound=%.3e)\n",
                      sycl_y, oracle_y, y_delta, y_bound, sycl_score, oracle_uv, uv_delta,
                      uv_bound);
    }
    mu_assert("motion_add_uv: SYCL Y-only score differs from fixed-point oracle",
              y_delta <= y_bound);
    mu_assert("motion_add_uv: SYCL Y+U+V score differs from fixed-point oracle",
              uv_delta <= uv_bound);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_motion_add_uv_increases_score);
    mu_run_test(test_motion_add_uv_fixed_oracle_parity);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
