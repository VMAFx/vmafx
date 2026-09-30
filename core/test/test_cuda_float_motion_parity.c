/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-0956 — float_motion CPU vs. CUDA parity test (round 4).
 *
 * The float-path motion extractor is implemented independently in
 * core/src/feature/float_motion.c (CPU) and
 * core/src/feature/cuda/float_motion_cuda.c (CUDA).
 *
 * Feature surface: both twins emit three features
 * (`VMAF_feature_motion_score`, `..._motion2_score`,
 * `..._motion3_score`). The CUDA twin emitted the first two only until
 * T-GPU-FLOAT-MOTION3-MISSING-2026-09-30; its motion3 is now the CPU's
 * host-side blend of motion2 (float_motion.c::motion_blend_clip), so all
 * three are compared.
 *
 * Without this test, a SIMD pivot on the CPU side or a kernel-grid
 * change on the CUDA side could silently shift the float-path motion
 * scores away from the CPU reference; the CHUG-extracted
 * `motion_mean`/`motion2_mean` columns feed every research-time
 * float-path model.
 *
 * Skip behaviour: emits "[skip: no CUDA device]" and passes when
 * vmaf_cuda_state_init fails (CPU-only CI lanes). Mirrors
 * test_cuda_motion3_parity.c.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
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
#define FIXTURE_BPC 8u
#define NUM_FRAMES 3u

/* ADR-0214 cross-backend tolerance (places=4 → 1e-4). */
#define PARITY_TOL 1e-4

/* Features emitted by both CPU `float_motion` and CUDA
 * `float_motion_cuda`. */
#define NUM_MOTION_FEATURES 3u
static const char *const MOTION_FEATURES[NUM_MOTION_FEATURES] = {
    "VMAF_feature_motion_score",
    "VMAF_feature_motion2_score",
    "VMAF_feature_motion3_score",
};

static int fill_fixture(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    /* Frame-dependent ramp so successive frames differ and produce a
     * non-zero motion score. Motion is luma-only — chroma planes
     * stay constant. */
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + frame_idx * 13u) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            memset(plane + row * pic->stride[p], 128, pic->w[p]);
        }
    }
    return 0;
}

/* Scores of every frame: [frame][feature]. */
typedef double MotionScores[NUM_FRAMES][NUM_MOTION_FEATURES];

/* Feed NUM_FRAMES fixture pairs and flush. */
static char *feed_frames(VmafContext *vmaf)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        mu_assert("fill_fixture(ref) failed", !fill_fixture(&ref, i));
        mu_assert("fill_fixture(dist) failed", !fill_fixture(&dist, i));
        mu_assert("vmaf_read_pictures failed", !vmaf_read_pictures(vmaf, &ref, &dist, i));
    }
    mu_assert("vmaf_read_pictures(EOS) failed", !vmaf_read_pictures(vmaf, NULL, NULL, 0));
    return NULL;
}

/* Every frame, so motion3 of frame 0 (from the first SAD) and of the last
 * frame (from the flush) are compared too. */
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
static char *run_feature(VmafContext *vmaf, const char *feature, MotionScores out)
{
    mu_assert("vmaf_use_feature failed", !vmaf_use_feature(vmaf, feature, NULL));
    char *msg = feed_frames(vmaf);
    return msg ? msg : read_scores(vmaf, out);
}

static char *run_cpu(MotionScores out)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("CPU: vmaf_init failed", !vmaf_init(&vmaf, cfg));
    char *msg = run_feature(vmaf, "float_motion", out);
    const int close_err = vmaf_close(vmaf);
    if (msg)
        return msg;
    mu_assert("CPU: vmaf_close failed", !close_err);
    return NULL;
}

static char *run_cuda(MotionScores out, int *skipped)
{
    *skipped = 0;
    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    if (vmaf_cuda_state_init(&cu_state, cuda_cfg) != 0 || cu_state == NULL) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        *skipped = 1;
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    char *msg = NULL;
    if (vmaf_init(&vmaf, cfg)) {
        msg = "CUDA: vmaf_init failed";
    } else if (vmaf_cuda_import_state(vmaf, cu_state)) {
        msg = "CUDA: vmaf_cuda_import_state failed";
    } else {
        msg = run_feature(vmaf, "float_motion_cuda", out);
    }
    /* The CUDA state is freed only after its context is closed (ADR-0157). */
    const int close_err = vmaf ? vmaf_close(vmaf) : 0;
    const int free_err = vmaf_cuda_state_free(cu_state);
    if (msg)
        return msg;
    mu_assert("CUDA: vmaf_close failed", !close_err);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !free_err);
    return NULL;
}

static char *test_float_motion_cpu_cuda_parity(void)
{
    MotionScores cpu_scores = {{0}};
    MotionScores cuda_scores = {{0}};
    int skipped = 0;

    char *msg = run_cpu(cpu_scores);
    if (msg)
        return msg;
    msg = run_cuda(cuda_scores, &skipped);
    if (msg || skipped)
        return msg;

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        for (unsigned m = 0; m < NUM_MOTION_FEATURES; m++) {
            mu_assert("CPU float_motion score is non-finite", isfinite(cpu_scores[i][m]));
            mu_assert("CUDA float_motion score is non-finite", isfinite(cuda_scores[i][m]));
            const double delta = fabs(cpu_scores[i][m] - cuda_scores[i][m]);
            if (delta > PARITY_TOL) {
                (void)fprintf(stderr,
                              "\nfloat_motion parity FAIL %s[%u]: cpu=%.8f cuda=%.8f delta=%.2e "
                              "tol=%.2e\n",
                              MOTION_FEATURES[m], i, cpu_scores[i][m], cuda_scores[i][m], delta,
                              PARITY_TOL);
            }
            mu_assert("float_motion CPU vs. CUDA delta exceeds places=4 tolerance (1e-4)",
                      delta <= PARITY_TOL);
        }
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_motion_cpu_cuda_parity);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
