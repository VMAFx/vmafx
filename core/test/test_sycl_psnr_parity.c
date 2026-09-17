/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * GPU-kernel coverage gap-fill — psnr CPU vs. SYCL parity test.
 *
 * The 8-bit integer PSNR sum-of-squared-error is computed by
 * integer_psnr.c (CPU) and by integer_psnr_sycl.cpp (SYCL kernel).
 * The SYCL backend previously had only motion3 / CAMBI parity gates;
 * a regression in the SYCL reduction kernel for the per-plane SSE
 * (e.g. work-group tiling, USM allocator stride) would have escaped
 * CI undetected and silently corrupted the psnr_{y,cb,cr} columns
 * used by Intel-Arc CHUG re-extracts.
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
#define PARITY_TOL 1e-4

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + salt * 23u) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                plane[row * pic->stride[p] + col] =
                    (uint8_t)((row * 5u + col + salt * 11u) & 0xFFu);
            }
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

// NOLINTNEXTLINE(readability-function-size): test scaffolding (ADR-0141 / ADR-0278) — the body walks the whole allocate / fill / run-CPU / run-SYCL / compare / free sequence in one place so a parity failure points at the exact stage that diverged; splitting it hides which assertion fired.
static char *run_cpu_psnr(double *y, double *cb, double *cr)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "psnr", NULL);
    mu_assert("CPU: vmaf_use_feature(psnr) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_y", y, 0u);
    mu_assert("CPU: psnr_y missing", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_cb", cb, 0u);
    mu_assert("CPU: psnr_cb missing", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_cr", cr, 0u);
    mu_assert("CPU: psnr_cr missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

// NOLINTNEXTLINE(readability-function-size): test scaffolding (ADR-0141 / ADR-0278) — the body walks the whole allocate / fill / run-CPU / run-SYCL / compare / free sequence in one place so a parity failure points at the exact stage that diverged; splitting it hides which assertion fired.
static char *run_sycl_psnr(double *y, double *cb, double *cr)
{
    *y = NAN;
    *cb = NAN;
    *cr = NAN;
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
    err = vmaf_use_feature(vmaf, "psnr_sycl", NULL);
    mu_assert("SYCL: vmaf_use_feature(psnr_sycl) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("SYCL: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("SYCL: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_y", y, 0u);
    mu_assert("SYCL: psnr_y missing", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_cb", cb, 0u);
    mu_assert("SYCL: psnr_cb missing", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_cr", cr, 0u);
    mu_assert("SYCL: psnr_cr missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);
    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

static char *check_pair(const char *label, double cpu, double gpu)
{
    if (isnan(gpu))
        return NULL;
    double delta = fabs(cpu - gpu);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\n%s parity FAIL: cpu=%.8f sycl=%.8f delta=%.2e tol=%.2e\n", label,
                      cpu, gpu, delta, PARITY_TOL);
    }
    mu_assert("psnr CPU vs. SYCL delta exceeds places=4 tolerance (1e-4)", delta <= PARITY_TOL);
    return NULL;
}

static char *test_psnr_sycl_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr_sycl");
    mu_assert("psnr_sycl extractor must be registered", fex != NULL);
    mu_assert("psnr_sycl name matches", !strcmp(fex->name, "psnr_sycl"));
    return NULL;
}

static char *test_psnr_cpu_sycl_parity(void)
{
    double cy = 0.0;
    double cb = 0.0;
    double cr = 0.0;
    double sy = NAN;
    double sb = NAN;
    double sr = NAN;
    char *msg = run_cpu_psnr(&cy, &cb, &cr);
    if (msg)
        return msg;
    msg = run_sycl_psnr(&sy, &sb, &sr);
    if (msg)
        return msg;
    msg = check_pair("psnr_y", cy, sy);
    if (msg)
        return msg;
    msg = check_pair("psnr_cb", cb, sb);
    if (msg)
        return msg;
    msg = check_pair("psnr_cr", cr, sr);
    if (msg)
        return msg;
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_psnr_sycl_registered);
    mu_run_test(test_psnr_cpu_sycl_parity);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
