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
 * ADR-1004 round 5 — speed_chroma CPU vs. HIP parity test.
 *
 * SpEED-QA chroma (Spatial Efficient Estimation of Degradation — chroma
 * variant) computes per-UV-channel prediction-error scores.  The CPU
 * extractor `speed_chroma` (`core/src/feature/speed.c:1335`) and its
 * HIP twin `speed_chroma_hip` (`core/src/feature/hip/speed_chroma_hip.c`)
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
 * Asserts `Speed_chroma_feature_speed_chroma_uv_score` at frame 0.
 * Fixture geometry: 768x432 YUV420P 8 bpc.  SpEED needs a 4× downscale
 * before 5×5 block analysis; 768x432 → 48×27 → 9×5 blocks (matching the
 * CUDA test_cuda_speed_chroma_parity.c fixture).
 *
 * Tolerance: places=4 (1e-4) per ADR-0214 cross-backend gate.
 * SpEED's QR / eigensolver runs on CPU for both backends; only the pixel
 * statistics (mean, covariance, backward-substitution) run on GPU.
 * The GPU paths use the same float arithmetic as the CPU scalar, so
 * places=4 is the appropriate budget.
 *
 * Skip behaviour:
 *   1. `vmaf_hip_state_init()` fails (no HIP/ROCm runtime or no device
 *      visible): emits `[skip: no HIP device]` and passes.
 *   2. `vmaf_use_feature()` or the first-frame submit returns `-ENOSYS`
 *      (library built with `enable_hipcc=false`, scaffold posture active
 *      per the `#ifndef HAVE_HIPCC` guard in `speed_chroma_hip.c`):
 *      emits `[skip: HIP scaffold ENOSYS]` and passes.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"

/* SpEED needs >= 4× downscale room plus 5×5 blocks; 768x432 gives
 * 192×108 after 2× prescale → 48×27 operating resolution → 9×5 blocks.
 * Matches test_cuda_speed_chroma_parity.c for cross-backend comparability. */
#ifndef FIXTURE_W
#define FIXTURE_W 768u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 432u
#endif
#define FIXTURE_BPC 8u
#define NUM_FRAMES 2u

/* ADR-0214 places=4 tolerance. */
#define PARITY_TOL 1e-4

static int fill_fixture(VmafPicture *pic, unsigned frame_idx, int distort)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] =
                (uint8_t)((row * 3u + col * 5u + frame_idx * 11u) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                const unsigned base = (row * 7u + col * 13u + frame_idx * 17u + p * 23u) & 0xFFu;
                const unsigned d = distort ? ((row + col) & 0x07u) : 0u;
                plane[row * pic->stride[p] + col] = (uint8_t)((base + d) & 0xFFu);
            }
        }
    }
    return 0;
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

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("vmaf_init failed", !err);

    if (use_hip) {
        err = vmaf_hip_import_state(vmaf, hip_state);
        mu_assert("vmaf_hip_import_state failed", !err);
    }

    err = vmaf_use_feature(vmaf, fex_name, NULL);
    if (use_hip && err == -ENOSYS) {
        (void)fprintf(stderr, "[skip: HIP scaffold ENOSYS] ");
        *skipped = 1;
        (void)vmaf_close(vmaf);
        vmaf_hip_state_free(&hip_state);
        return NULL;
    }
    mu_assert("vmaf_use_feature failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_fixture(&ref, i, 0);
        mu_assert("fill_fixture(ref) failed", !err);
        err = fill_fixture(&dist, i, 1);
        mu_assert("fill_fixture(dist) failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (use_hip && err == -ENOSYS) {
            (void)fprintf(stderr, "[skip: HIP scaffold ENOSYS on submit] ");
            *skipped = 1;
            (void)vmaf_close(vmaf);
            vmaf_hip_state_free(&hip_state);
            return NULL;
        }
        mu_assert("vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "Speed_chroma_feature_speed_chroma_uv_score", out_score,
                                      0u);
    mu_assert("vmaf_feature_score_at_index(speed_chroma_uv) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("vmaf_close failed", !err);

    if (hip_state)
        vmaf_hip_state_free(&hip_state);
    return NULL;
}

static char *test_speed_chroma_cpu_hip_parity(void)
{
    double cpu_score = NAN;
    double hip_score = NAN;
    int skipped = 0;

    char *msg = drive("speed_chroma", 0, &cpu_score, &skipped);
    if (msg)
        return msg;

    msg = drive("speed_chroma_hip", 1, &hip_score, &skipped);
    if (msg)
        return msg;

    if (skipped || isnan(hip_score))
        return NULL;

    const double delta = fabs(cpu_score - hip_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\nspeed_chroma parity FAIL: cpu=%.8f hip=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, hip_score, delta, PARITY_TOL);
    }
    mu_assert("speed_chroma CPU vs. HIP delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_speed_chroma_cpu_hip_parity);
    return NULL;
}
