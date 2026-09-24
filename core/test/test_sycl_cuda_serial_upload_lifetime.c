/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * BUG-040 regression: in a build that contains both CUDA and SYCL,
 * vmaf_read_pictures() must finish the asynchronous SYCL host upload before
 * its serial cleanup releases the caller's pictures.
 *
 * The test drives the public serial ingestion path with the real psnr_sycl
 * extractor. Each distorted picture has a release callback that overwrites
 * its 4K luma plane immediately before returning the buffer to libvmaf's
 * pool. With the required wait-before-release ordering, the device already
 * owns the original all-zero plane and every identical-input PSNR is capped
 * at 60 dB. If CUDA's host-cleanup branch returns before the SYCL wait, the
 * poison races the DMA and at least one frame scores below the cap.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"
#include "picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

#define FIXTURE_W 3840u
#define FIXTURE_H 2160u
#define N_FRAMES 48u
#define EXPECTED_IDENTICAL_PSNR 60.0

typedef struct ReleaseProbe {
    void *original_cookie;
    int (*original_release)(VmafPicture *pic, void *cookie);
    unsigned fired;
} ReleaseProbe;

static int poison_then_release(VmafPicture *pic, void *cookie)
{
    ReleaseProbe *probe = cookie;
    probe->fired++;
    if (pic->data[0] != NULL)
        memset(pic->data[0], 0xFF, (size_t)pic->stride[0] * pic->h[0]);
    return probe->original_release(pic, probe->original_cookie);
}

static int arm_release_probe(VmafPicture *pic, ReleaseProbe *probe)
{
    if (pic == NULL || pic->priv == NULL || probe == NULL)
        return -EINVAL;
    VmafPicturePrivate *priv = pic->priv;
    probe->original_cookie = priv->cookie;
    probe->original_release = priv->release_picture;
    probe->fired = 0u;
    priv->cookie = probe;
    priv->release_picture = poison_then_release;
    return 0;
}

static int alloc_zero_picture(VmafPicture *pic)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV400P, 8u, FIXTURE_W, FIXTURE_H);
    if (!err)
        memset(pic->data[0], 0, (size_t)pic->stride[0] * pic->h[0]);
    return err;
}

static int feed_poisoned_frame(VmafContext *vmaf, unsigned index, ReleaseProbe *probe)
{
    VmafPicture ref = {0};
    VmafPicture dist = {0};
    int err = alloc_zero_picture(&ref);
    if (err)
        return err;
    err = alloc_zero_picture(&dist);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    err = arm_release_probe(&dist, probe);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        (void)vmaf_picture_unref(&dist);
        return err;
    }
    err = vmaf_read_pictures(vmaf, &ref, &dist, index);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        (void)vmaf_picture_unref(&dist);
    }
    return err;
}

static int collect_scores(VmafContext *vmaf, double scores[N_FRAMES])
{
    int err = vmaf_read_pictures(vmaf, NULL, NULL, 0u);
    for (unsigned i = 0u; i < N_FRAMES && !err; i++)
        err = vmaf_feature_score_at_index(vmaf, "psnr_y", &scores[i], i);
    return err;
}

static int run_serial_lifetime_probe(double scores[N_FRAMES], unsigned *release_count)
{
    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    int err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err != 0 || sycl_state == NULL)
        return -ENODEV;

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .n_threads = 0u};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    if (!err)
        err = vmaf_sycl_import_state(vmaf, sycl_state);
    if (!err)
        err = vmaf_use_feature(vmaf, "psnr_sycl", NULL);

    ReleaseProbe probes[N_FRAMES] = {0};
    for (unsigned i = 0u; i < N_FRAMES && !err; i++)
        err = feed_poisoned_frame(vmaf, i, &probes[i]);
    if (!err)
        err = collect_scores(vmaf, scores);

    *release_count = 0u;
    for (unsigned i = 0u; i < N_FRAMES; i++)
        *release_count += probes[i].fired;

    const int close_err = vmaf != NULL ? vmaf_close(vmaf) : 0;
    vmaf_sycl_state_free(&sycl_state);
    return err ? err : close_err;
}

static char *test_serial_cleanup_waits_before_host_release()
{
    double scores[N_FRAMES] = {0.0};
    unsigned release_count = 0u;
    const int err = run_serial_lifetime_probe(scores, &release_count);
    if (err == -ENODEV) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        mu_skipped = 1;
        return NULL;
    }
    mu_assert("CUDA+SYCL serial lifetime probe failed", err == 0);
    mu_assert("every distorted picture must be released", release_count == N_FRAMES);

    unsigned corrupted = 0u;
    for (unsigned i = 0u; i < N_FRAMES; i++) {
        if (fabs(scores[i] - EXPECTED_IDENTICAL_PSNR) > 1e-12) {
            (void)fprintf(stderr, "\nframe %u: expected %.1f dB, got %.17g", i,
                          EXPECTED_IDENTICAL_PSNR, scores[i]);
            corrupted++;
        }
    }
    mu_assert("host picture was released before its SYCL upload completed", corrupted == 0u);
    return NULL;
}

char *run_tests()
{
    mu_run_test(test_serial_cleanup_waits_before_host_release);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
