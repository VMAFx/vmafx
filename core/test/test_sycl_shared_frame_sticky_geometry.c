/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Verification test for T-SYCL-SHARED-FRAME-STICKY-GEOMETRY-2026-09-29 and
 * T-SYCL-SHARED-FRAME-GEOMETRY-REUSE-2026-09-29.
 *
 * When a VmafSyclState is reused sequentially by multiple VmafContext instances
 * with different frame geometries (e.g. 64x48 then 128x96 then 32x24), the
 * shared frame and chroma buffers must reallocate to match each context's
 * geometry instead of retaining the first context's buffers.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

#define TOL 1e-4

static uint8_t test_pattern(unsigned plane, unsigned row, unsigned col, unsigned salt)
{
    const unsigned mix = (row * (3u + plane)) ^ (col * (5u + salt));
    return (uint8_t)((mix + salt * 29u) & 0xFFu);
}

static int fill_plane(VmafPicture *pic, unsigned plane, unsigned salt)
{
    uint8_t *data = (uint8_t *)pic->data[plane];
    for (unsigned r = 0; r < pic->h[plane]; r++) {
        for (unsigned c = 0; c < pic->w[plane]; c++) {
            data[(size_t)r * (size_t)pic->stride[plane] + c] = test_pattern(plane, r, c, salt);
        }
    }
    return 0;
}

static int fill_test_pic(VmafPicture *pic, unsigned w, unsigned h, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8u, w, h);
    if (err)
        return err;
    for (unsigned p = 0; p < 3u; p++) {
        err = fill_plane(pic, p, salt + p * 7u);
        if (err) {
            (void)vmaf_picture_unref(pic);
            return err;
        }
    }
    return 0;
}

static int feed_test_frames(VmafContext *vmaf, unsigned w, unsigned h)
{
    for (unsigned f = 0; f < 2u; f++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_test_pic(&ref, w, h, f * 17u);
        if (err)
            return err;
        err = fill_test_pic(&dist, w, h, f * 17u + 1u);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            return err;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, f);
        if (err)
            return err;
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

static int run_pass(VmafSyclState *sycl_state, unsigned w, unsigned h, double *y, double *cb,
                    double *cr)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (err)
        return err;

    if (sycl_state) {
        err = vmaf_sycl_import_state(vmaf, sycl_state);
        if (err) {
            (void)vmaf_close(vmaf);
            return err;
        }
        err = vmaf_use_feature(vmaf, "psnr_sycl", NULL);
    } else {
        err = vmaf_use_feature(vmaf, "psnr", NULL);
    }
    if (err) {
        (void)vmaf_close(vmaf);
        return err;
    }

    err = feed_test_frames(vmaf, w, h);
    if (err) {
        (void)vmaf_close(vmaf);
        return err;
    }

    err = vmaf_feature_score_at_index(vmaf, "psnr_y", y, 0u);
    err |= vmaf_feature_score_at_index(vmaf, "psnr_cb", cb, 0u);
    err |= vmaf_feature_score_at_index(vmaf, "psnr_cr", cr, 0u);
    (void)vmaf_close(vmaf);
    return err;
}

static char *check_geom(VmafSyclState *state, unsigned w, unsigned h)
{
    double cpu_y = 0.0, cpu_cb = 0.0, cpu_cr = 0.0;
    double gpu_y = 0.0, gpu_cb = 0.0, gpu_cr = 0.0;
    mu_assert("run_pass CPU", !run_pass(NULL, w, h, &cpu_y, &cpu_cb, &cpu_cr));
    mu_assert("run_pass GPU", !run_pass(state, w, h, &gpu_y, &gpu_cb, &gpu_cr));

    if (fabs(cpu_y - gpu_y) > TOL) {
        (void)fprintf(stderr, "\n  %ux%u psnr_y mismatch: cpu=%.6f gpu=%.6f diff=%.6f\n", w, h,
                      cpu_y, gpu_y, fabs(cpu_y - gpu_y));
        return "psnr_y mismatch between CPU and SYCL";
    }
    if (fabs(cpu_cb - gpu_cb) > TOL) {
        (void)fprintf(stderr, "\n  %ux%u psnr_cb mismatch: cpu=%.6f gpu=%.6f diff=%.6f\n", w, h,
                      cpu_cb, gpu_cb, fabs(cpu_cb - gpu_cb));
        return "psnr_cb mismatch between CPU and SYCL";
    }
    if (fabs(cpu_cr - gpu_cr) > TOL) {
        (void)fprintf(stderr, "\n  %ux%u psnr_cr mismatch: cpu=%.6f gpu=%.6f diff=%.6f\n", w, h,
                      cpu_cr, gpu_cr, fabs(cpu_cr - gpu_cr));
        return "psnr_cr mismatch between CPU and SYCL";
    }
    return NULL;
}

static char *test_sticky_geometry(void)
{
    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    int err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err != 0 || !sycl_state) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return NULL;
    }

    /* Context 1: 64x48 */
    mu_assert_msg(check_geom(sycl_state, 64u, 48u));

    /* Context 2: 128x96 (larger geometry on the reused state) */
    mu_assert_msg(check_geom(sycl_state, 128u, 96u));

    /* Context 3: 32x24 (smaller geometry on the reused state) */
    mu_assert_msg(check_geom(sycl_state, 32u, 24u));

    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_sticky_geometry);
    return NULL;
}
