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
 * ADR-0956 — speed_chroma_cuda smoke / sanity test (round 4).
 *
 * `speed_chroma_cuda` is a CUDA-only feature extractor — there is no
 * CPU twin that emits the same `Speed_chroma_feature_*_score` family
 * (the closest CPU surface, `speed_qa`, computes a different scalar).
 * A CPU-vs-CUDA parity test is therefore not the right tool here.
 *
 * What this test gates instead: the extractor must register, initialize
 * on a CUDA device, run a multi-frame YUV420P fixture through the full
 * filter → eigendecomp → kernel pipeline (ADR-0567), and emit finite,
 * sane scores for all three channels (u, v, uv) at frame index 1.
 *
 * Drift risk this test catches: a kernel grid change, a covariance
 * matrix degenerate case, or a missing initialization on the host-side
 * eigendecomp path that quietly returns NaN/Inf rather than a numeric
 * score. Without this gate, the round-2 NaN-pollution audit
 * (ADR-0886) had to rely on full-pipeline integration tests that take
 * minutes; this smoke test runs in single-digit seconds.
 *
 * Fixture geometry: 640x360 luma → 320x180 chroma → 20x11 operating
 * resolution after 2^4 decimation (ADR-0567), 4x2 = 8 blocks per
 * channel. Large enough to exercise the speed_means/cov/score kernels
 * without singular-matrix warnings; small enough for fast CI.
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
 * chroma plane is 320x180, operating resolution after 2^4 decimation
 * is 20x11 → 4x2 = 8 blocks per channel. */
#define FIXTURE_W 640u
#define FIXTURE_H 360u
#define FIXTURE_BPC 8u
#define NUM_FRAMES 3u

#define NUM_CHANNELS 3u
static const char *const SPEED_CHROMA_FEATURES[NUM_CHANNELS] = {
    "Speed_chroma_feature_speed_chroma_u_score",
    "Speed_chroma_feature_speed_chroma_v_score",
    "Speed_chroma_feature_speed_chroma_uv_score",
};

/* Per-frame fixture: structured luma + chroma content that exercises
 * both channels (u and v differ deterministically so the per-channel
 * scores are non-zero). */
static int fill_fixture(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    /* Luma: frame-dependent ramp. */
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + frame_idx * 11u) & 0xFFu);
        }
    }
    /* Chroma U: spatially varying so eigendecomp covariance is non-singular. */
    uint8_t *u = (uint8_t *)pic->data[1];
    for (unsigned row = 0; row < pic->h[1]; row++) {
        for (unsigned col = 0; col < pic->w[1]; col++) {
            u[row * pic->stride[1] + col] =
                (uint8_t)(128u + ((row * 3u + col * 2u + frame_idx) & 0x3Fu));
        }
    }
    /* Chroma V: distinct pattern from U. */
    uint8_t *v = (uint8_t *)pic->data[2];
    for (unsigned row = 0; row < pic->h[2]; row++) {
        for (unsigned col = 0; col < pic->w[2]; col++) {
            v[row * pic->stride[2] + col] =
                (uint8_t)(128u + ((row * 2u + col * 5u + frame_idx * 7u) & 0x3Fu));
        }
    }
    return 0;
}

static char *run_speed_chroma_smoke(double *out_scores, int *skipped)
{
    *skipped = 0;
    for (unsigned c = 0; c < NUM_CHANNELS; c++)
        out_scores[c] = NAN;

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

    err = vmaf_use_feature(vmaf, "speed_chroma_cuda", NULL);
    mu_assert("CUDA: vmaf_use_feature(speed_chroma_cuda) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_fixture(&ref, i);
        mu_assert("CUDA: fill_fixture(ref) failed", !err);
        err = fill_fixture(&dist, i);
        mu_assert("CUDA: fill_fixture(dist) failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CUDA: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CUDA: vmaf_read_pictures(EOS) failed", !err);

    for (unsigned c = 0; c < NUM_CHANNELS; c++) {
        err = vmaf_feature_score_at_index(vmaf, SPEED_CHROMA_FEATURES[c], &out_scores[c], 1u);
        mu_assert("CUDA: vmaf_feature_score_at_index(speed_chroma[c], idx=1) failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}

static char *test_speed_chroma_cuda_smoke(void)
{
    double scores[NUM_CHANNELS] = {0};
    int skipped = 0;

    char *msg = run_speed_chroma_smoke(scores, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;

    for (unsigned c = 0; c < NUM_CHANNELS; c++) {
        if (!isfinite(scores[c])) {
            (void)fprintf(stderr, "\nspeed_chroma_cuda smoke FAIL: %s = %.8f (non-finite)\n",
                          SPEED_CHROMA_FEATURES[c], scores[c]);
        }
        mu_assert("speed_chroma_cuda score is non-finite", isfinite(scores[c]));
    }

    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_speed_chroma_cuda_smoke);
    return NULL;
}
