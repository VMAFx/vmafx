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
 * SYCL kernel coverage round 3 — float motion CPU vs. SYCL parity test
 * (ADR-0946).
 *
 * The float motion extractor is implemented by float_motion.c (CPU
 * scalar / SIMD via motion.c) and by
 * float_motion_sycl.cpp::vmaf_fex_float_motion_sycl (SYCL temporal-SAD
 * + blended-motion2 reduction). The existing motion3 parity gate
 * (test_sycl_motion3_parity.c) covers the integer motion3 path; the
 * float variant uses a different precision class, a different blend
 * factor surface, and a different add-uv config branch — it needs its
 * own parity gate.
 *
 * The headline score for this parity gate is
 * VMAF_feature_motion2_score at frame index 1 (the first frame where
 * the temporal blend has a previous-frame reference).
 *
 * Skip behaviour: if vmaf_sycl_state_init() fails (no oneAPI runtime
 * or no device visible) the test emits "[skip: no SYCL device]" and
 * passes, mirroring test_sycl_motion3_parity.c.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

/* Fixture matches the motion_v2 round-2 sizing — large enough for a
 * stable SAD reduction on Intel Arc, small enough to keep the test
 * under the fast-suite budget. */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#define FIXTURE_BPC 8u
#define NUM_FRAMES 2u
#define PARITY_TOL 1e-4

static int fill_pic(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            /* Frame-dependent offset → non-zero temporal SAD. */
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

static char *run_cpu(double *score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "float_motion", NULL);
    mu_assert("CPU: vmaf_use_feature(float_motion) failed", !err);
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        err = fill_pic(&ref, i);
        mu_assert("CPU: fill_pic(ref) failed", !err);
        err = fill_pic(&dist, i);
        mu_assert("CPU: fill_pic(dist) failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CPU: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "VMAF_feature_motion2_score", score, 1u);
    mu_assert("CPU: VMAF_feature_motion2_score at idx=1 missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_sycl(double *score)
{
    *score = NAN;
    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    int err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err != 0 || sycl_state == NULL) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return NULL;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("SYCL: vmaf_init failed", !err);
    err = vmaf_sycl_import_state(vmaf, sycl_state);
    mu_assert("SYCL: vmaf_sycl_import_state failed", !err);
    err = vmaf_use_feature(vmaf, "float_motion_sycl", NULL);
    mu_assert("SYCL: vmaf_use_feature(float_motion_sycl) failed", !err);
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        err = fill_pic(&ref, i);
        mu_assert("SYCL: fill_pic(ref) failed", !err);
        err = fill_pic(&dist, i);
        mu_assert("SYCL: fill_pic(dist) failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("SYCL: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("SYCL: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "VMAF_feature_motion2_score", score, 1u);
    mu_assert("SYCL: VMAF_feature_motion2_score at idx=1 missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);
    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

static char *test_float_motion_sycl_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_motion_sycl");
    mu_assert("float_motion_sycl extractor must be registered", fex != NULL);
    mu_assert("float_motion_sycl name matches", !strcmp(fex->name, "float_motion_sycl"));
    return NULL;
}

static char *test_float_motion_cpu_sycl_parity(void)
{
    double cpu_score = 0.0;
    double sycl_score = NAN;
    char *msg = run_cpu(&cpu_score);
    if (msg)
        return msg;
    msg = run_sycl(&sycl_score);
    if (msg)
        return msg;
    if (isnan(sycl_score))
        return NULL;
    double delta = fabs(cpu_score - sycl_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\nfloat_motion2 parity FAIL: cpu=%.8f sycl=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, sycl_score, delta, PARITY_TOL);
    }
    mu_assert("float_motion2 CPU vs. SYCL delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_motion_sycl_registered);
    mu_run_test(test_float_motion_cpu_sycl_parity);
    return NULL;
}
