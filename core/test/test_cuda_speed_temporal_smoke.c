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
 * ADR-0956 — speed_temporal_cuda smoke / sanity test (round 4).
 *
 * `speed_temporal_cuda` is a CUDA-only feature extractor — there is no
 * CPU twin that emits the `Speed_temporal_feature_speed_temporal_score`
 * scalar (the closest CPU surface, `speed_qa`, computes a different
 * spatial+temporal sum). A CPU-vs-CUDA parity test is therefore not the
 * right tool here.
 *
 * What this test gates instead: the extractor must register, initialize
 * on a CUDA device, run a multi-frame YUV420P fixture through the full
 * filter → eigendecomp → kernel pipeline (ADR-0567 — luma operating
 * resolution, distinct from the chroma path), and emit a finite, sane
 * `speed_temporal` score at frame index 1.
 *
 * Drift risk this test catches: a kernel grid change, a covariance
 * matrix degenerate case, or a missing initialization on the host-side
 * eigendecomp path that quietly returns NaN/Inf. The temporal path
 * additionally exercises the prev-frame difference buffer, which has
 * its own lifecycle (TEMPORAL extractor flag).
 *
 * Fixture geometry: 640x360 luma → operating resolution 40x22 after
 * 2^4 decimation (ADR-0567), 8x4 = 32 blocks. Large enough to exercise
 * the speed_means/cov/score kernels without singular-matrix warnings;
 * small enough for fast CI.
 *
 * Skip behaviour: emits "[skip: no CUDA device]" and passes when
 * vmaf_cuda_state_init fails. Mirrors test_cuda_motion3_parity.c.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

/* Large enough for non-degenerate eigendecomp; small enough for fast CI.
 * Luma plane is 640x360, operating resolution after 2^4 decimation is
 * 40x22 → 8x4 = 32 blocks. */
#define FIXTURE_W 640u
#define FIXTURE_H 360u
#define FIXTURE_BPC 8u
#define NUM_FRAMES 3u

#define SPEED_TEMPORAL_FEATURE "Speed_temporal_feature_speed_temporal_score"

/* Per-frame fixture: frame-dependent luma ramp so the temporal-diff
 * buffer has non-zero content (without it, the covariance matrix
 * collapses to all-zero and the eigendecomp warns and zeros the score). */
static int fill_ref(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

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

/* Distorted: ref + deterministic per-pixel noise to make the
 * speed_temporal score non-trivial. */
static int fill_dist(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const unsigned base = (row + col + frame_idx * 13u) & 0xFFu;
            const unsigned noise = ((row * 3u + col * 2u + frame_idx) % 11u);
            y[row * pic->stride[0] + col] = (uint8_t)((base + noise) & 0xFFu);
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

static char *run_speed_temporal_smoke(double *out_score, int *skipped)
{
    *skipped = 0;
    *out_score = NAN;

    int err = 0;
    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    err = vmaf_cuda_state_init(&cu_state, cuda_cfg);
    if (err != 0 || cu_state == NULL) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        *skipped = 1;
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CUDA: vmaf_init failed", !err);

    err = vmaf_cuda_import_state(vmaf, cu_state);
    mu_assert("CUDA: vmaf_cuda_import_state failed", !err);

    err = vmaf_use_feature(vmaf, "speed_temporal_cuda", NULL);
    mu_assert("CUDA: vmaf_use_feature(speed_temporal_cuda) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_ref(&ref, i);
        mu_assert("CUDA: fill_ref failed", !err);
        err = fill_dist(&dist, i);
        mu_assert("CUDA: fill_dist failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CUDA: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CUDA: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, SPEED_TEMPORAL_FEATURE, out_score, 1u);
    mu_assert("CUDA: vmaf_feature_score_at_index(speed_temporal, idx=1) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}

static char *test_speed_temporal_cuda_smoke(void)
{
    double score = NAN;
    int skipped = 0;

    char *msg = run_speed_temporal_smoke(&score, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;

    if (!isfinite(score)) {
        (void)fprintf(stderr, "\nspeed_temporal_cuda smoke FAIL: score = %.8f (non-finite)\n",
                      score);
    }
    mu_assert("speed_temporal_cuda score is non-finite", isfinite(score));

    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_speed_temporal_cuda_smoke);
    return NULL;
}
