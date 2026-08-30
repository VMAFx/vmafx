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
 * Metal kernel parity — integer_vif CPU vs. Metal (ADR-0214 cross-backend
 * gate; ADR-0421 Metal scaffolding).
 *
 * The integer (fixed-point) VIF extractor is registered under the name `vif`
 * on the CPU (core/src/feature/integer_vif.c) and emits the per-scale feature
 * keys VMAF_integer_feature_vif_scale{0..3}_score. The Metal twin
 * `integer_vif_metal` (core/src/feature/metal/integer_vif_metal.mm +
 *  core/src/feature/metal/integer_vif.metal) mirrors the proven
 * float_vif_metal 4-scale separable-Gaussian pyramid dispatch but swaps the
 * float arithmetic for the CPU integer reference's int64 fixed-point moment
 * accumulators and the uint16 log2 look-up table (VIF_LOG2_TABLE_SIZE = 32768
 * entries) regenerated identically host-side and uploaded as a device buffer.
 *
 * This test runs the CPU `vif` extractor against `integer_vif_metal` over a
 * 2-frame YUV420P fixture and asserts that all four per-scale scores at frame
 * index 1 agree within places=4 (1e-4). The integer moments are computed
 * exactly on the GPU (int64 accumulators, same rounding shifts as the CPU);
 * the only non-integer divergence is the int64->float final cast of the
 * per-scale num/den (matched here by reproducing the CPU's float-precision
 * num[0]/den[0] writes), well inside the 1e-4 ADR-0214 gate.
 *
 * Skip behaviour: -ENODEV from `vmaf_metal_state_init` -> clean skip (exit 0,
 * "[skip: no Metal device]") on Linux / Windows / Intel Mac.
 *
 * Cross-references:
 *   - core/src/feature/metal/integer_vif_metal.mm
 *   - core/src/feature/metal/integer_vif.metal
 *   - core/src/feature/integer_vif.c            (CPU reference, feature "vif")
 *   - core/test/test_metal_float_vif_parity.c   (float twin)
 *   - core/test/test_metal_integer_ssim_parity.c (integer sibling)
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_metal.h"
#include "libvmaf/picture.h"

#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u
#define NUM_FRAMES 2u

/* integer kernels target the tight places=4 (1e-4) cross-backend bound from
 * ADR-0214, matching the CUDA/SYCL integer_vif parity tests. */
#define PARITY_TOL 1e-4

/* The CPU integer_vif extractor (feature name "vif") emits these per-scale
 * keys; the Metal twin emits the same keys (identical provided_features). */
static const char *const VIF_SCALE_FEATURES[] = {
    "VMAF_integer_feature_vif_scale0_score",
    "VMAF_integer_feature_vif_scale1_score",
    "VMAF_integer_feature_vif_scale2_score",
    "VMAF_integer_feature_vif_scale3_score",
};
#define NUM_VIF_SCALES 4u

static int fill_ref(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + frame_idx * 7u) & 0xFFu);
        }
    }
    /* Distinct chroma to surface accidental chroma reads (VIF is luma-only). */
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                plane[row * pic->stride[p] + col] = (uint8_t)((row * 3u + col + p * 17u) & 0xFFu);
            }
        }
    }
    return 0;
}

static int fill_dist(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const unsigned base = (row + col + frame_idx * 7u) & 0xFFu;
            const unsigned noise = ((row * 2u + col + frame_idx * 3u) % 13u);
            y[row * pic->stride[0] + col] = (uint8_t)((base + noise) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                plane[row * pic->stride[p] + col] =
                    (uint8_t)((row + col * 5u + p * 23u + frame_idx) & 0xFFu);
            }
        }
    }
    return 0;
}

static char *run_cpu(double *out_scores)
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "vif", NULL);
    mu_assert("CPU: vmaf_use_feature(vif) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_ref(&ref, i);
        mu_assert("CPU: fill_ref failed", !err);
        err = fill_dist(&dist, i);
        mu_assert("CPU: fill_dist failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CPU: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    for (unsigned s = 0; s < NUM_VIF_SCALES; s++) {
        err = vmaf_feature_score_at_index(vmaf, VIF_SCALE_FEATURES[s], &out_scores[s], 1u);
        mu_assert("CPU: vmaf_feature_score_at_index(vif_scale[i], idx=1) failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_metal(double *out_scores, int *skipped)
{
    *skipped = 0;
    for (unsigned s = 0; s < NUM_VIF_SCALES; s++)
        out_scores[s] = NAN;

    int err = 0;
    VmafMetalConfiguration mcfg = {.device_index = -1, .flags = 0};
    VmafMetalState *mstate = NULL;
    err = vmaf_metal_state_init(&mstate, mcfg);
    if (err != 0 || mstate == NULL) {
        (void)fprintf(stderr, "[skip: no Metal device] ");
        *skipped = 1;
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("Metal: vmaf_init failed", !err);

    err = vmaf_metal_import_state(vmaf, mstate);
    mu_assert("Metal: vmaf_metal_import_state failed", !err);

    err = vmaf_use_feature(vmaf, "integer_vif_metal", NULL);
    mu_assert("Metal: vmaf_use_feature(integer_vif_metal) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_ref(&ref, i);
        mu_assert("Metal: fill_ref failed", !err);
        err = fill_dist(&dist, i);
        mu_assert("Metal: fill_dist failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("Metal: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("Metal: vmaf_read_pictures(EOS) failed", !err);

    for (unsigned s = 0; s < NUM_VIF_SCALES; s++) {
        err = vmaf_feature_score_at_index(vmaf, VIF_SCALE_FEATURES[s], &out_scores[s], 1u);
        mu_assert("Metal: vmaf_feature_score_at_index(vif_scale[i], idx=1) failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("Metal: vmaf_close failed", !err);
    vmaf_metal_state_free(&mstate);
    return NULL;
}

static char *test_integer_vif_cpu_metal_parity(void)
{
    double cpu_scores[NUM_VIF_SCALES] = {0};
    double metal_scores[NUM_VIF_SCALES] = {0};
    int skipped = 0;

    char *msg = run_cpu(cpu_scores);
    if (msg)
        return msg;
    msg = run_metal(metal_scores, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;

    for (unsigned s = 0; s < NUM_VIF_SCALES; s++) {
        mu_assert("CPU integer_vif scale score is non-finite", isfinite(cpu_scores[s]));
        mu_assert("Metal integer_vif scale score is non-finite", isfinite(metal_scores[s]));

        const double delta = fabs(cpu_scores[s] - metal_scores[s]);
        if (delta > PARITY_TOL) {
            (void)fprintf(
                stderr,
                "\ninteger_vif parity FAIL scale=%u: cpu=%.8f metal=%.8f delta=%.2e tol=%.2e\n", s,
                cpu_scores[s], metal_scores[s], delta, PARITY_TOL);
        }
        mu_assert("integer_vif CPU vs. Metal scale delta exceeds places=4 tolerance (1e-4)",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_integer_vif_cpu_metal_parity);
    return NULL;
}
