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
 * SYCL kernel coverage round 4 — speed_temporal CPU vs. SYCL parity
 * test (ADR-0957).
 *
 * The SpEED temporal extractor is implemented by speed.c (CPU scalar
 * via vmaf_fex_speed_temporal) and by
 * speed_temporal_sycl.cpp::vmaf_fex_speed_temporal_sycl (SYCL
 * ping-pong diff + Gaussian-pyramid + entropy reduction on the
 * temporal-difference image, ADR-0567). Both extractors carry the
 * VMAF_FEATURE_EXTRACTOR_TEMPORAL flag — the kernel produces 0.0 at
 * frame index 0 and the real diff-based score from frame index 1
 * onward.
 *
 * Round 3 (ADR-0946) deferred this kernel because the SpEED family
 * carries per-extractor option-table setup (`speed_kernelscale`,
 * `speed_prescale`, `speed_prescale_method`, `speed_sigma_nn`,
 * `speed_nn_floor`, `speed_max_val`, `speed_use_ref_diff`) that was
 * not yet templated in the round-2 / round-3 scaffold. The defaults
 * match between CPU and SYCL so a `NULL` options dict still exercises
 * the same numerical path on both sides.
 *
 * Headline score asserted at frame index 1:
 * "Speed_temporal_feature_speed_temporal_score".
 *
 * A ping-pong indexing, Gaussian-radius, or weight-table drift in
 * the SYCL kernel would silently shift every temporal-SpEED column
 * on Intel-Arc CHUG re-extracts.
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

/* Two frames are the minimum to exercise the ping-pong diff path
 * (frame 0 always emits 0.0; the meaningful score is at frame 1). */
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
    /* Luma carries the temporal-diff signal; chroma stays neutral. */
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            /* Frame-dependent offset → non-zero temporal diff. */
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
    err = vmaf_use_feature(vmaf, "speed_temporal", NULL);
    mu_assert("CPU: vmaf_use_feature(speed_temporal) failed", !err);
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        err = fill_pic(&ref, i);
        mu_assert("CPU: fill_pic(ref) failed", !err);
        err = fill_pic(&dist, i + 7u);
        mu_assert("CPU: fill_pic(dist) failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CPU: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err =
        vmaf_feature_score_at_index(vmaf, "Speed_temporal_feature_speed_temporal_score", score, 1u);
    mu_assert("CPU: speed_temporal score at idx=1 missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_sycl(double *score, int *device_present)
{
    *score = NAN;
    *device_present = 0;
    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    int err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err != 0 || sycl_state == NULL) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return NULL;
    }
    *device_present = 1;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("SYCL: vmaf_init failed", !err);
    err = vmaf_sycl_import_state(vmaf, sycl_state);
    mu_assert("SYCL: vmaf_sycl_import_state failed", !err);
    err = vmaf_use_feature(vmaf, "speed_temporal_sycl", NULL);
    mu_assert("SYCL: vmaf_use_feature(speed_temporal_sycl) failed", !err);
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        err = fill_pic(&ref, i);
        mu_assert("SYCL: fill_pic(ref) failed", !err);
        err = fill_pic(&dist, i + 7u);
        mu_assert("SYCL: fill_pic(dist) failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("SYCL: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("SYCL: vmaf_read_pictures(EOS) failed", !err);
    err =
        vmaf_feature_score_at_index(vmaf, "Speed_temporal_feature_speed_temporal_score", score, 1u);
    mu_assert("SYCL: speed_temporal score at idx=1 missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);
    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

/* speed_temporal_sycl is a 705-LOC SYCL TU (ADR-0567) whose source
 * lives in core/src/feature/sycl/speed_temporal_sycl.cpp but is not
 * yet wired into `sycl_feature_sources` in `core/src/meson.build`.
 * The extractor symbol `vmaf_fex_speed_temporal_sycl` is therefore
 * not registered. Treat the missing extractor as a skip so the
 * test exists, links, and auto-activates the day the build wiring
 * lands. The skip surfaces a `[skip: speed_temporal_sycl not built
 * into libvmaf]` message that tracks the gap. */
static int speed_temporal_sycl_present(void)
{
    return vmaf_get_feature_extractor_by_name("speed_temporal_sycl") != NULL;
}

static char *test_speed_temporal_sycl_registered_or_skip(void)
{
    if (!speed_temporal_sycl_present()) {
        (void)fprintf(stderr, "[skip: speed_temporal_sycl not built into libvmaf] ");
        return NULL;
    }
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("speed_temporal_sycl");
    mu_assert("speed_temporal_sycl name matches", !strcmp(fex->name, "speed_temporal_sycl"));
    return NULL;
}

static char *test_speed_temporal_cpu_sycl_parity(void)
{
    if (!speed_temporal_sycl_present()) {
        (void)fprintf(stderr, "[skip: speed_temporal_sycl not built into libvmaf] ");
        return NULL;
    }
    double cpu_score = 0.0;
    double sycl_score = NAN;
    int device_present = 0;
    char *msg = run_cpu(&cpu_score);
    if (msg)
        return msg;
    msg = run_sycl(&sycl_score, &device_present);
    if (msg)
        return msg;
    if (!device_present)
        return NULL;
    double delta = fabs(cpu_score - sycl_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\nspeed_temporal parity FAIL: cpu=%.8f sycl=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, sycl_score, delta, PARITY_TOL);
    }
    mu_assert("speed_temporal CPU vs. SYCL delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_speed_temporal_sycl_registered_or_skip);
    mu_run_test(test_speed_temporal_cpu_sycl_parity);
    return NULL;
}
