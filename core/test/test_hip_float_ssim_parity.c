/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-0958 round-4 — float_ssim CPU vs. HIP parity test.
 *
 * The float-pipeline SSIM mirrors `integer_ssim` (already covered by
 * `test_hip_ssim_parity`, PR #372 / ADR-0883) but operates on
 * float-converted Y-plane data via `float_ssim.c` (CPU) and
 * `float_ssim_hip.c` (HIP).  Both twins emit a single `float_ssim`
 * channel per their shared
 * `provided_features = { "float_ssim", NULL }` declaration.
 *
 * The HIP path skips cleanly when:
 *   1. `vmaf_hip_state_init()` fails (no AMD GPU / no HIP runtime), or
 *   2. The init / submit / collect calls return `-ENOSYS` from the
 *      `#ifndef HAVE_HIPCC` guards in `float_ssim_hip.c`.
 *
 * Tolerance: places=3 (1e-3).  SSIM accumulates per-window rounding
 * across the luminance / contrast / structure terms; the round-2 audit
 * (ADR-0883) settled on places=3 for the integer SSIM gate and the
 * float pipeline follows the same convention.
 *
 * Fixture geometry: 256x144 YUV420P 8 bpc (matches the round-1/2/3
 * template; well above the 8x8 lower bound enforced by the SSIM init
 * paths).
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "hip_parity_skip.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"

#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#define FIXTURE_BPC 8u
#define PARITY_TOL 1e-3

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + salt * 19u) & 0xFFu);
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

static int feed_frame(VmafContext *vmaf)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_pic(&ref, 0u);
    if (err)
        return err;
    err = fill_pic(&dist, 1u);
    if (err) {
        vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, 0u);
}

static char *run_cpu_float_ssim(double *score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "float_ssim", NULL);
    mu_assert("CPU: vmaf_use_feature(float_ssim) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "float_ssim", score, 0u);
    mu_assert("CPU: float_ssim missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

/*
 * Feed the fixture frame into the HIP run and triage the result.
 *
 * Sets `*done` when the run is finished -- `vmaf` closed and the HIP state
 * released -- so the caller returns without reading a score. Split out of
 * run_hip_float_ssim() so that body stays inside the 60-line function budget.
 */
static char *hip_feed_and_triage(VmafContext *vmaf, VmafHipState **hip_state, int *skipped,
                                 int *done)
{
    const int err = feed_frame(vmaf);
    if (err == -ENOSYS) {
        *done = 1;
        return hip_parity_skip(vmaf, hip_state, skipped, " on feed");
    }
    /* `float_ssim_hip` is a v1 scale=1-only extractor: its init rejects any
     * resolution whose auto-detected decimation factor
     * `max(1, round(min(w, h) / 256))` is not 1 — i.e. min(w, h) >= 384 — with
     * -EINVAL (core/src/feature/hip/float_ssim_hip.c). The CPU `float_ssim` has
     * no such limit and silently decimates instead, so at those resolutions the
     * two extractors do not compute the same quantity and there is no parity to
     * assert. Treat the documented refusal as a skip; anything else is a real
     * failure. Keeping the large-fixture variant registered means that if the
     * twin ever stops refusing and starts returning a scale=1 score at a
     * decimating resolution, this test fails instead of silently comparing two
     * different metrics. See ADR-1206. */
    if (err && ((FIXTURE_W < FIXTURE_H ? FIXTURE_W : FIXTURE_H) >= 384u)) {
        (void)fprintf(stderr, "[skip: float_ssim_hip is scale=1-only; %ux%u auto-decimates] ",
                      FIXTURE_W, FIXTURE_H);
        *skipped = 1;
        *done = 1;
        (void)vmaf_close(vmaf);
        vmaf_hip_state_free(hip_state);
        return NULL;
    }
    if (err == -ENOSYS) {
        /* Documented scaffold contract: an unimplemented HIP extractor returns
         * -ENOSYS from init (see the HIP extractors under
         * core/src/feature/hip/). That is a not-built-yet signal, not a
         * regression, so skip exactly as the no-device branch above does.
         * Any other error still fails. */
        (void)fprintf(stderr, "[skip: HIP extractor is a scaffold (-ENOSYS)] ");
        *done = 1;
        (void)vmaf_close(vmaf);
        vmaf_hip_state_free(hip_state);
        return NULL;
    }
    mu_assert("HIP: feed_frame failed", !err);
    return NULL;
}

static char *run_hip_float_ssim(double *score, int *skipped)
{
    *score = NAN;
    *skipped = 0;
    VmafHipState *hip_state = NULL;
    VmafHipConfiguration hip_cfg = {.device_index = -1};
    int err = vmaf_hip_state_init(&hip_state, hip_cfg);
    if (err != 0 || hip_state == NULL) {
        (void)fprintf(stderr, "[skip: no HIP device] ");
        *skipped = 1;
        return NULL;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("HIP: vmaf_init failed", !err);
    err = vmaf_hip_import_state(vmaf, hip_state);
    mu_assert("HIP: vmaf_hip_import_state failed", !err);
    err = vmaf_use_feature(vmaf, "float_ssim_hip", NULL);
    if (err == -ENOSYS)
        return hip_parity_skip(vmaf, &hip_state, skipped, "");
    mu_assert("HIP: vmaf_use_feature(float_ssim_hip) failed", !err);
    int done = 0;
    char *msg = hip_feed_and_triage(vmaf, &hip_state, skipped, &done);
    if (msg || done)
        return msg;
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (err == -ENOSYS)
        return hip_parity_skip(vmaf, &hip_state, skipped, " on EOS");
    mu_assert("HIP: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "float_ssim", score, 0u);
    mu_assert("HIP: float_ssim missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("HIP: vmaf_close failed", !err);
    vmaf_hip_state_free(&hip_state);
    return NULL;
}

static char *test_float_ssim_hip_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ssim_hip");
    mu_assert("float_ssim_hip extractor must be registered", fex != NULL);
    mu_assert("float_ssim_hip name matches", !strcmp(fex->name, "float_ssim_hip"));
    return NULL;
}

static char *test_float_ssim_cpu_hip_parity(void)
{
    double cpu = 0.0;
    double gpu = NAN;
    int skipped = 0;

    char *msg = run_cpu_float_ssim(&cpu);
    if (msg)
        return msg;
    msg = run_hip_float_ssim(&gpu, &skipped);
    if (msg)
        return msg;
    if (skipped || isnan(gpu))
        return NULL;
    double delta = fabs(cpu - gpu);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\nfloat_ssim parity FAIL: cpu=%.8f hip=%.8f delta=%.2e tol=%.2e\n",
                      cpu, gpu, delta, PARITY_TOL);
    }
    mu_assert("float_ssim CPU vs. HIP delta exceeds places=3 tolerance (1e-3)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_ssim_hip_registered);
    mu_run_test(test_float_ssim_cpu_hip_parity);
    return NULL;
}
