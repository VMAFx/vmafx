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
 * Metal kernel parity — integer_adm CPU vs. Metal (ADR-0214 cross-backend
 * gate; ADR-0421 first-kernel Metal scaffolding).
 *
 * The integer (fixed-point) ADM extractor is registered under the name `adm`
 * on the CPU (core/src/feature/integer_adm.c) and emits the aggregate
 * `VMAF_integer_feature_adm2_score` plus the four `integer_adm_scale[0..3]`
 * sub-scores. The Metal twin `integer_adm_metal`
 * (core/src/feature/metal/integer_adm_metal.mm +
 *  core/src/feature/metal/integer_adm.metal) mirrors the proven
 * float_adm_metal six-stage DWT -> CSF -> decouple -> CM dispatch but swaps
 * the float arithmetic for the CPU integer reference's bit-exact fixed-point
 * arithmetic (int16 bands at scale 0, int32 "i4" bands at scales 1-3, the
 * div_Q_factor / get_best15_from32 decouple, the per-scale i_rfactor / shift
 * tables, and the conclude_adm_cm / conclude_adm_csf_den int64 cube-recovery
 * host reduction).
 *
 * This test runs the CPU `adm` extractor against `integer_adm_metal` over a
 * two-frame YUV420P fixture and asserts the integer-ADM aggregate plus the
 * four per-scale scores match within places=4 (1e-4). Integer kernels are
 * expected to be tight / near-bit-exact; the residual the 1e-4 ADR-0214 gate
 * bounds is the float32 conclude-stage cube-root pooling done in MSL vs.
 * double on the CPU.
 *
 * Skip behaviour: -ENODEV from `vmaf_metal_state_init` -> clean skip on
 * Linux / Windows / Intel Mac.
 *
 * Cross-references:
 *   - core/test/test_metal_float_adm_parity.c (float twin)
 *   - core/test/test_metal_integer_ssim_parity.c (integer sibling)
 *   - core/src/feature/metal/integer_adm_metal.mm
 *   - core/src/feature/integer_adm.c
 *   - core/src/feature/cuda/integer_adm/ (bit-exact GPU reference)
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

/* >= 17x17 (CPU integer_adm min) and large enough for a meaningful 4-scale
 * pyramid; matches the float_adm parity fixture geometry. */
#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u
#define NUM_FRAMES 2u
#define PARITY_TOL 1e-4

static const char *const ADM_FEATURES[] = {
    "VMAF_integer_feature_adm2_score",
    "integer_adm_scale0",
    "integer_adm_scale1",
    "integer_adm_scale2",
    "integer_adm_scale3",
};
#define NUM_ADM_FEATURES (sizeof(ADM_FEATURES) / sizeof(ADM_FEATURES[0]))

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
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            memset(plane + row * pic->stride[p], 128, pic->w[p]);
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
            const unsigned noise = ((row * 3u + col * 2u + frame_idx) % 13u);
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

static char *run_cpu(double *out_scores)
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "adm", NULL);
    mu_assert("CPU: vmaf_use_feature(adm) failed", !err);

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

    for (unsigned m = 0; m < NUM_ADM_FEATURES; m++) {
        err = vmaf_feature_score_at_index(vmaf, ADM_FEATURES[m], &out_scores[m], 1u);
        mu_assert("CPU: vmaf_feature_score_at_index(adm[i], idx=1) failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_metal(double *out_scores, int *skipped)
{
    *skipped = 0;
    for (unsigned m = 0; m < NUM_ADM_FEATURES; m++)
        out_scores[m] = NAN;

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

    err = vmaf_use_feature(vmaf, "integer_adm_metal", NULL);
    mu_assert("Metal: vmaf_use_feature(integer_adm_metal) failed", !err);

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

    for (unsigned m = 0; m < NUM_ADM_FEATURES; m++) {
        err = vmaf_feature_score_at_index(vmaf, ADM_FEATURES[m], &out_scores[m], 1u);
        mu_assert("Metal: vmaf_feature_score_at_index(adm[i], idx=1) failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("Metal: vmaf_close failed", !err);
    vmaf_metal_state_free(&mstate);
    return NULL;
}

static char *test_integer_adm_cpu_metal_parity(void)
{
    double cpu_scores[NUM_ADM_FEATURES] = {0};
    double metal_scores[NUM_ADM_FEATURES] = {0};
    int skipped = 0;

    char *msg = run_cpu(cpu_scores);
    if (msg)
        return msg;
    msg = run_metal(metal_scores, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;

    for (unsigned m = 0; m < NUM_ADM_FEATURES; m++) {
        mu_assert("CPU integer_adm score is non-finite", isfinite(cpu_scores[m]));
        mu_assert("Metal integer_adm score is non-finite", isfinite(metal_scores[m]));

        const double delta = fabs(cpu_scores[m] - metal_scores[m]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\ninteger_adm parity FAIL %s: cpu=%.8f metal=%.8f delta=%.2e tol=%.2e\n",
                          ADM_FEATURES[m], cpu_scores[m], metal_scores[m], delta, PARITY_TOL);
        }
        mu_assert("integer_adm CPU vs. Metal delta exceeds places=4 tolerance (1e-4)",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_integer_adm_cpu_metal_parity);
    return NULL;
}
