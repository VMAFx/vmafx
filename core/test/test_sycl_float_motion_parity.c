/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_motion CPU vs. SYCL parity test (ADR-0946, exact since ADR-1411).
 *
 * The float-path motion extractor is implemented independently in
 * core/src/feature/float_motion.c (CPU) and
 * core/src/feature/sycl/float_motion_sycl.cpp (SYCL).
 *
 * Feature surface: the twin emits `VMAF_feature_motion_score` and
 * `VMAF_feature_motion2_score`; `motion3` stays on the CPU extractor
 * (T-GPU-FLOAT-MOTION3-MISSING-2026-09-30), so those two are compared, on
 * every frame.
 *
 * The comparison is exact (ADR-1409, ADR-1411): the twin's blur is the CPU's
 * convolution without FMA contraction (ADR-1367), and its SAD is added in the
 * CPU's order, one fp32 accumulator per row and one over the rows. Any other
 * reduction shape differs in the low bits, which the noise fixture shows
 * most: its row sums pass 2^11 and round at every step. The 8-bit and the
 * 16-bit sample paths of the blur kernel both run.
 *
 * Skip behaviour: emits "[skip: no SYCL device]" and passes when
 * vmaf_sycl_state_init fails. Mirrors test_sycl_motion3_parity.c.
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#define NUM_FRAMES 3u

/* Features emitted by both CPU `float_motion` and SYCL
 * `float_motion_sycl`. */
#define NUM_MOTION_FEATURES 2u
static const char *const MOTION_FEATURES[NUM_MOTION_FEATURES] = {
    "VMAF_feature_motion_score",
    "VMAF_feature_motion2_score",
};

enum FixtureKind {
    /* Diagonal ramp moving 13 codes per frame. */
    FIXTURE_RAMP,
    /* Uncorrelated samples, new ones every frame: the largest SAD a frame
     * pair can have, so the fp32 running sums round the most. */
    FIXTURE_NOISE,
};

typedef struct Fixture {
    enum FixtureKind kind;
    unsigned bpc;
} Fixture;

/* Numerical Recipes' 32-bit linear congruential step. */
static uint32_t lcg_next(uint32_t state)
{
    return state * 1664525u + 1013904223u;
}

/* Luma sample of the fixture at (`row`, `col`), in 0..255. `noise` is the
 * running generator state of a noise fixture. */
static unsigned fixture_code(const Fixture *fx, unsigned frame_idx, unsigned row, unsigned col,
                             uint32_t *noise)
{
    if (fx->kind == FIXTURE_NOISE) {
        *noise = lcg_next(*noise);
        return *noise >> 24u;
    }
    return (row + col + frame_idx * 13u) & 0xFFu;
}

/* Write the luma plane: 8-bit codes, scaled to the sample depth above 8. */
static void fill_luma(VmafPicture *pic, const Fixture *fx, unsigned frame_idx)
{
    uint32_t noise = 0x9E3779B9u ^ (frame_idx * 0x85EBCA6Bu);
    const unsigned shift = fx->bpc - 8u;
    for (unsigned row = 0; row < pic->h[0]; row++) {
        uint8_t *line = (uint8_t *)pic->data[0] + row * pic->stride[0];
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const unsigned code = fixture_code(fx, frame_idx, row, col, &noise);
            if (fx->bpc > 8u) {
                /* Low bits vary too, so the 1/scaler conversion is exercised. */
                const uint16_t sample = (uint16_t)((code << shift) | (col & ((1u << shift) - 1u)));
                memcpy(line + (size_t)col * sizeof(sample), &sample, sizeof(sample));
            } else {
                line[col] = (uint8_t)code;
            }
        }
    }
}

static int fill_fixture(VmafPicture *pic, const Fixture *fx, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, fx->bpc, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    /* Motion is luma-only: the chroma planes stay zero. */
    fill_luma(pic, fx, frame_idx);
    for (unsigned p = 1; p < 3; p++) {
        const size_t line_bytes = (size_t)pic->w[p] * ((fx->bpc > 8u) ? 2u : 1u);
        for (unsigned row = 0; row < pic->h[p]; row++) {
            memset((uint8_t *)pic->data[p] + row * pic->stride[p], 0, line_bytes);
        }
    }
    return 0;
}

/* Scores of every frame: [frame][feature]. */
typedef double MotionScores[NUM_FRAMES][NUM_MOTION_FEATURES];

/* Feed NUM_FRAMES fixture pairs and flush. */
static char *feed_frames(VmafContext *vmaf, const Fixture *fx)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        mu_assert("fill_fixture(ref) failed", !fill_fixture(&ref, fx, i));
        mu_assert("fill_fixture(dist) failed", !fill_fixture(&dist, fx, i));
        mu_assert("vmaf_read_pictures failed", !vmaf_read_pictures(vmaf, &ref, &dist, i));
    }
    mu_assert("vmaf_read_pictures(EOS) failed", !vmaf_read_pictures(vmaf, NULL, NULL, 0));
    return NULL;
}

/* Every frame, so motion2 of the last frame (from the flush) is compared
 * too. */
static char *read_scores(VmafContext *vmaf, MotionScores out)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        for (unsigned m = 0; m < NUM_MOTION_FEATURES; m++) {
            mu_assert("vmaf_feature_score_at_index failed",
                      !vmaf_feature_score_at_index(vmaf, MOTION_FEATURES[m], &out[i][m], i));
        }
    }
    return NULL;
}

/* Run `feature` over the fixture in `vmaf` and read every score back. */
static char *run_feature(VmafContext *vmaf, const char *feature, const Fixture *fx,
                         MotionScores out)
{
    mu_assert("vmaf_use_feature failed", !vmaf_use_feature(vmaf, feature, NULL));
    char *msg = feed_frames(vmaf, fx);
    return msg ? msg : read_scores(vmaf, out);
}

static char *run_cpu(const Fixture *fx, MotionScores out)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("CPU: vmaf_init failed", !vmaf_init(&vmaf, cfg));
    char *msg = run_feature(vmaf, "float_motion", fx, out);
    const int close_err = vmaf_close(vmaf);
    if (msg)
        return msg;
    mu_assert("CPU: vmaf_close failed", !close_err);
    return NULL;
}

static char *run_sycl(const Fixture *fx, MotionScores out, int *skipped)
{
    *skipped = 0;
    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    if (vmaf_sycl_state_init(&sycl_state, sycl_cfg) != 0 || sycl_state == NULL) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        *skipped = 1;
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    char *msg = NULL;
    if (vmaf_init(&vmaf, cfg)) {
        msg = "SYCL: vmaf_init failed";
    } else if (vmaf_sycl_import_state(vmaf, sycl_state)) {
        msg = "SYCL: vmaf_sycl_import_state failed";
    } else {
        msg = run_feature(vmaf, "float_motion_sycl", fx, out);
    }
    /* The context is closed before the state it imported is freed. */
    const int close_err = vmaf ? vmaf_close(vmaf) : 0;
    vmaf_sycl_state_free(&sycl_state);
    if (msg)
        return msg;
    mu_assert("SYCL: vmaf_close failed", !close_err);
    return NULL;
}

/* Bit patterns, so that -0.0 / 0.0 or two NaNs cannot pass as equal. */
static int same_bits(double a, double b)
{
    uint64_t ua;
    uint64_t ub;
    memcpy(&ua, &a, sizeof(ua));
    memcpy(&ub, &b, sizeof(ub));
    return ua == ub;
}

/* The twin's scores on `fx` are the CPU extractor's, bit for bit. */
static char *check_fixture(const Fixture *fx, const char *label)
{
    MotionScores cpu_scores = {{0}};
    MotionScores sycl_scores = {{0}};
    int skipped = 0;

    char *msg = run_cpu(fx, cpu_scores);
    if (msg)
        return msg;
    msg = run_sycl(fx, sycl_scores, &skipped);
    if (msg || skipped)
        return msg;

    int differing = 0;
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        for (unsigned m = 0; m < NUM_MOTION_FEATURES; m++) {
            mu_assert("CPU float_motion score is non-finite", isfinite(cpu_scores[i][m]));
            if (same_bits(cpu_scores[i][m], sycl_scores[i][m]))
                continue;
            differing++;
            (void)fprintf(stderr, "\nfloat_motion %s %s[%u]: cpu=%.17g sycl=%.17g delta=%.3e\n",
                          label, MOTION_FEATURES[m], i, cpu_scores[i][m], sycl_scores[i][m],
                          fabs(cpu_scores[i][m] - sycl_scores[i][m]));
        }
    }
    /* A fixture that scored 0 everywhere would compare nothing. */
    mu_assert("the fixture has no motion", cpu_scores[1][0] > 0.0);
    mu_assert("float_motion_sycl does not return the CPU extractor's scores bit for bit",
              differing == 0);
    return NULL;
}

static char *test_float_motion_sycl_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_motion_sycl");
    mu_assert("float_motion_sycl extractor must be registered", fex != NULL);
    mu_assert("float_motion_sycl name matches", !strcmp(fex->name, "float_motion_sycl"));
    return NULL;
}

static char *test_float_motion_ramp_8bit(void)
{
    const Fixture fx = {FIXTURE_RAMP, 8u};
    return check_fixture(&fx, "ramp 8-bit");
}

static char *test_float_motion_noise_8bit(void)
{
    const Fixture fx = {FIXTURE_NOISE, 8u};
    return check_fixture(&fx, "noise 8-bit");
}

static char *test_float_motion_noise_10bit(void)
{
    const Fixture fx = {FIXTURE_NOISE, 10u};
    return check_fixture(&fx, "noise 10-bit");
}

static char *test_float_motion_noise_12bit(void)
{
    const Fixture fx = {FIXTURE_NOISE, 12u};
    return check_fixture(&fx, "noise 12-bit");
}

char *run_tests(void)
{
    mu_run_test(test_float_motion_sycl_registered);
    mu_run_test(test_float_motion_ramp_8bit);
    mu_run_test(test_float_motion_noise_8bit);
    mu_run_test(test_float_motion_noise_10bit);
    mu_run_test(test_float_motion_noise_12bit);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
