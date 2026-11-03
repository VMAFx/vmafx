/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-1004 round 5 — speed_temporal CPU vs. HIP parity test.
 *
 * SpEED-QA temporal (Spatial Efficient Estimation of Degradation — temporal
 * variant) computes luma prediction-error scores across frames.  The CPU
 * extractor `speed_temporal` (`core/src/feature/speed.c:1559`) and its
 * HIP twin `speed_temporal_hip` (`core/src/feature/hip/speed_temporal_hip.c`)
 * share the CPU-side matrix algebra via `speed_internal.c` while the
 * tile-parallel GPU work (mean, covariance, indterm, score) runs as
 * HSACO device kernels.
 *
 * The round-4 ADR-0958 investigation surfaced a latent link defect:
 * `speed_internal_init_dimensions` / `speed_internal_float_stride` were
 * declared in `speed_internal.h` but had no `.c` implementation.  That
 * defect was resolved in a follow-up PR (ADR-0964 / PR #465) that added
 * `core/src/feature/speed_internal.c`.  This test is the parity gate
 * deferred from round 4 — now unblocked.
 *
 * Asserts `Speed_temporal_feature_speed_temporal_score` at frame 1
 * (frame 0 emits a forced-zero score by spec because no prior frame exists).
 * Fixture geometry: 768x432 YUV420P 8 bpc, 3 frames.  Matches the CUDA
 * test_cuda_speed_temporal_parity.c fixture.
 *
 * Tolerance: none.
 * Since ADR-1477 the comparison is an equality: speed.c evaluates Netflix's
 * fp64 statements (libvmaf/src/feature/speed.c of Netflix/vmaf 9e48141b:
 * `1.0 / sqrt(1 + t * t)` at 418 and 423, the `log2()` of update_entropy()
 * at 802 and of get_speed_score() at 897 to 928), and the twin reproduces
 * each of them, the rotation on the device (feature/speed_givens.h) and the
 * logarithms on the host with the CPU extractor's own C library
 * (speed_internal_gpu_tail_scores()).
 * The test allowed 1e-4 (places=4, ADR-0214) before.
 *
 * Skip behaviour:
 *   1. `vmaf_hip_state_init()` fails (no HIP/ROCm runtime or no device
 *      visible): emits `[skip: no HIP device]` and passes.
 *   2. `vmaf_use_feature()` or the first-frame submit returns `-ENOSYS`
 *      (library built with `enable_hipcc=false`, scaffold posture active
 *      per the `#ifndef HAVE_HIPCC` guard in `speed_temporal_hip.c`):
 *      emits `[skip: HIP scaffold ENOSYS]` and passes.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"
#include "float_bits.h"

#include "hip_parity_skip.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): this is a
 * C23 translation unit, but the required MSVC C lane does not provide the C
 * nullptr spelling clang-tidy proposes. Keep the portable C API form under
 * ADR-1138. */

/* speed_temporal needs >= 2 frames (frame 0 is forced-zero; frame 1 carries
 * the first real score).  3 frames gives a non-trivial inter-frame delta
 * and matches the CUDA speed_temporal test geometry. */
#ifndef FIXTURE_W
#define FIXTURE_W 768u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 432u
#endif
#define FIXTURE_BPC 8u
#define NUM_FRAMES 3u

static int fill_fixture(VmafPicture *pic, unsigned frame_idx, int distort)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const unsigned base = (row * 3u + col * 5u + frame_idx * 11u) & 0xFFu;
            const unsigned d = distort ? ((row + col) & 0x07u) : 0u;
            y[row * pic->stride[0] + col] = (uint8_t)((base + d) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            (void)memset(plane + row * pic->stride[p], 128, pic->w[p]);
        }
    }
    return 0;
}

static char *feed_temporal_frames(VmafContext *vmaf, int use_hip, VmafHipState **hip_state,
                                  int *skipped)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_fixture(&ref, i, 0);
        mu_assert("fill_fixture(ref) failed", !err);
        err = fill_fixture(&dist, i, 1);
        mu_assert("fill_fixture(dist) failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (use_hip && err == -ENOSYS) {
            return hip_parity_skip(vmaf, hip_state, skipped, " on submit");
        }
        mu_assert("vmaf_read_pictures failed", !err);
    }
    const int err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("vmaf_read_pictures(EOS) failed", !err);
    return NULL;
}

static char *init_temporal_vmaf(VmafContext **vmaf_out, const char *fex_name, int use_hip,
                                VmafHipState **hip_state, int *skipped)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    int err = vmaf_init(vmaf_out, cfg);
    mu_assert("vmaf_init failed", !err);

    if (use_hip) {
        err = vmaf_hip_import_state(*vmaf_out, *hip_state);
        mu_assert("vmaf_hip_import_state failed", !err);
    }

    err = vmaf_use_feature(*vmaf_out, fex_name, NULL);
    if (use_hip && err == -ENOSYS) {
        return hip_parity_skip(*vmaf_out, hip_state, skipped, "");
    }
    mu_assert("vmaf_use_feature failed", !err);
    return NULL;
}

/* Run `fex_name` over NUM_FRAMES synthetic frames.
 * Returns NULL on success; leaves *out_score as NAN if skipped.
 * *skipped is set to 1 if the HIP device was unavailable or if the
 * HIP scaffold returned -ENOSYS (enable_hipcc=false). */
static char *drive(const char *fex_name, int use_hip, double *out_score, int *skipped)
{
    *out_score = NAN;
    *skipped = 0;
    VmafHipState *hip_state = NULL;

    if (use_hip) {
        VmafHipConfiguration hip_cfg = {.device_index = -1};
        int rc = vmaf_hip_state_init(&hip_state, hip_cfg);
        if (rc != 0 || hip_state == NULL) {
            (void)fprintf(stderr, "[skip: no HIP device] ");
            *skipped = 1;
            return NULL;
        }
    }

    VmafContext *vmaf = NULL;
    char *msg = init_temporal_vmaf(&vmaf, fex_name, use_hip, &hip_state, skipped);
    if (msg || *skipped) {
        return msg;
    }

    msg = feed_temporal_frames(vmaf, use_hip, &hip_state, skipped);
    if (msg || *skipped) {
        return msg;
    }

    /* Frame 0 emits a forced-zero score; read frame 1 for the real delta. */
    int err = vmaf_feature_score_at_index(vmaf, "Speed_temporal_feature_speed_temporal_score",
                                          out_score, 1u);
    mu_assert("vmaf_feature_score_at_index(speed_temporal, idx=1) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("vmaf_close failed", !err);

    if (hip_state) {
        vmaf_hip_state_free(&hip_state);
    }
    return NULL;
}

static char *test_speed_temporal_cpu_hip_parity(void)
{
    double cpu_score = NAN;
    double hip_score = NAN;
    int skipped = 0;

    char *msg = drive("speed_temporal", 0, &cpu_score, &skipped);
    if (msg)
        return msg;

    msg = drive("speed_temporal_hip", 1, &hip_score, &skipped);
    if (msg)
        return msg;

    if (skipped || isnan(hip_score)) {
        return NULL;
    }

    if (!vmaf_test_identical_f64(cpu_score, hip_score)) {
        (void)fprintf(stderr, "\nspeed_temporal parity FAIL: cpu=%.17g hip=%.17g delta=%.2e\n",
                      cpu_score, hip_score, fabs(cpu_score - hip_score));
    }
    mu_assert("speed_temporal_hip does not return the CPU extractor's score",
              vmaf_test_identical_f64(cpu_score, hip_score));
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_speed_temporal_cpu_hip_parity);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
