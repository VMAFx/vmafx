/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-0883 round-2 / ADR-0564 — integer SSIM CPU vs. HIP parity test.
 *
 * SSIM is computed by integer_ssim.c (CPU, extractor `ssim`) and by
 * integer_ssim_hip.c + integer_ssim/integer_ssim_score.hip (HIP, extractor
 * `integer_ssim_hip`); both emit the `ssim` feature. The HIP twin runs the
 * CPU's 9-tap int64 algorithm, so every frame must agree within places=4
 * (1e-4), the gate the CUDA, SYCL and Metal integer_ssim twins use
 * (ADR-0214). Measured deltas are around 1e-14: only the order in which the
 * per-pixel terms are summed differs.
 *
 * The fixture is N_FRAMES frames fetched from a picture pool sized like the
 * CLI's, so a picture buffer is refilled with the next frame as soon as
 * vmaf_read_pictures() lets go of it. That is what exposed a HIP staging
 * race: an asynchronous upload that was still reading a picture after
 * submit() returned scored some frames against the next frame's samples.
 * A single-frame fixture cannot see it.
 *
 * meson registers this TU several times: at 8 bpc (the 256x144 default and
 * a 960x540 variant), at 10 bpc (-DFIXTURE_BPC=10u, which runs the 16-bit
 * horizontal kernel) and at an odd 577x323 size, where the window is
 * truncated at a partial right and bottom block.
 *
 * Skip behaviour: no HIP device, or a build without device kernels
 * (-ENOSYS from init), prints "[skip: ...]" and exits as skipped.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this test mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#ifndef FIXTURE_BPC
#define FIXTURE_BPC 8u
#endif
#define N_FRAMES 8u
/* 2 * (threads + 1) + 1 with no worker threads: the CLI's pool size. */
#define POOL_PICTURES 3u
#define PARITY_TOL 1e-4

/* Bit-depth generic sample writer: an 8-bit ramp in the high bits and a
 * second pattern in the low (bpc - 8) bits. */
static void put_luma(VmafPicture *pic, unsigned row, unsigned col, unsigned v8, unsigned low_seed)
{
#if FIXTURE_BPC > 8u
    uint16_t *y = (uint16_t *)((uint8_t *)pic->data[0] + (size_t)row * (size_t)pic->stride[0]);
    const unsigned low_mask = (1u << (FIXTURE_BPC - 8u)) - 1u;
    y[col] = (uint16_t)(((v8 & 0xFFu) << (FIXTURE_BPC - 8u)) | (low_seed & low_mask));
#else
    uint8_t *y = (uint8_t *)pic->data[0] + (size_t)row * (size_t)pic->stride[0];
    (void)low_seed;
    y[col] = (uint8_t)(v8 & 0xFFu);
#endif
}

static void fill_chroma_grey(VmafPicture *pic)
{
    for (unsigned p = 1u; p < 3u; p++) {
        for (unsigned row = 0u; row < pic->h[p]; row++) {
            for (unsigned col = 0u; col < pic->w[p]; col++) {
#if FIXTURE_BPC > 8u
                uint16_t *c =
                    (uint16_t *)((uint8_t *)pic->data[p] + (size_t)row * (size_t)pic->stride[p]);
                c[col] = (uint16_t)(128u << (FIXTURE_BPC - 8u));
#else
                uint8_t *c = (uint8_t *)pic->data[p] + (size_t)row * (size_t)pic->stride[p];
                c[col] = 128u;
#endif
            }
        }
    }
}

/* Frame `frame` of the reference (salt 0) or the distorted clip (salt 1).
 * The content moves every frame and the distortion offset grows with the
 * frame index, so each frame has its own score and a frame scored against
 * another frame's samples shows up as a delta. */
static int fetch_frame(VmafContext *vmaf, VmafPicture *pic, unsigned frame, unsigned salt)
{
    const int err = vmaf_fetch_preallocated_picture(vmaf, pic);
    if (err)
        return err;
    const unsigned offset = frame * 11u + salt * (17u + 3u * frame);
    for (unsigned row = 0u; row < pic->h[0]; row++) {
        for (unsigned col = 0u; col < pic->w[0]; col++)
            put_luma(pic, row, col, row + col + offset, row * 7u + col * 3u + salt);
    }
    fill_chroma_grey(pic);
    return 0;
}

static int feed_frames(VmafContext *vmaf)
{
    const VmafPictureConfiguration pool = {
        .pic_params =
            {
                .w = FIXTURE_W,
                .h = FIXTURE_H,
                .bpc = FIXTURE_BPC,
                .pix_fmt = VMAF_PIX_FMT_YUV420P,
            },
        .pic_cnt = POOL_PICTURES,
    };
    int err = vmaf_preallocate_pictures(vmaf, pool);
    for (unsigned f = 0u; f < N_FRAMES && !err; f++) {
        VmafPicture ref;
        VmafPicture dist;
        err = fetch_frame(vmaf, &ref, f, 0u);
        if (err)
            break;
        err = fetch_frame(vmaf, &dist, f, 1u);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            break;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, f);
    }
    return err;
}

/* Runs `extractor` over the fixture and reads the `ssim` score of every
 * frame into `scores`. Returns 0, or the error of the first failing call. */
static int run_ssim(VmafContext *vmaf, const char *extractor, double *scores)
{
    int err = vmaf_use_feature(vmaf, extractor, NULL);
    if (!err)
        err = feed_frames(vmaf);
    if (!err)
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    for (unsigned f = 0u; f < N_FRAMES && !err; f++)
        err = vmaf_feature_score_at_index(vmaf, "ssim", &scores[f], f);
    return err;
}

static char *run_cpu_ssim(double *scores)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = run_ssim(vmaf, "ssim", scores);
    mu_assert("CPU: ssim extraction failed", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

/* Sets *ran to false, with the test marked skipped, when there is no HIP
 * device or the build has no device kernels. */
static char *run_hip_ssim(double *scores, int *ran)
{
    *ran = 0;
    VmafHipState *hip_state = NULL;
    VmafHipConfiguration hip_cfg = {.device_index = -1};
    int err = vmaf_hip_state_init(&hip_state, hip_cfg);
    if (err != 0 || hip_state == NULL) {
        (void)fprintf(stderr, "[skip: no HIP device] ");
        mu_skipped = 1;
        return NULL;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("HIP: vmaf_init failed", !err);
    err = vmaf_hip_import_state(vmaf, hip_state);
    mu_assert("HIP: vmaf_hip_import_state failed", !err);
    err = run_ssim(vmaf, "integer_ssim_hip", scores);
    if (err == -ENOSYS) {
        /* Documented scaffold contract: a HIP extractor built without
         * enable_hipcc returns -ENOSYS from init. Not a regression. */
        (void)fprintf(stderr, "[skip: HIP extractor is a scaffold (-ENOSYS)] ");
        mu_skipped = 1;
    } else {
        mu_assert("HIP: integer_ssim_hip extraction failed", !err);
        *ran = 1;
    }
    err = vmaf_close(vmaf);
    mu_assert("HIP: vmaf_close failed", !err);
    vmaf_hip_state_free(&hip_state);
    return NULL;
}

static char *test_ssim_hip_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("integer_ssim_hip");
    mu_assert("integer_ssim_hip extractor must be registered", fex != NULL);
    mu_assert("integer_ssim_hip name matches", !strcmp(fex->name, "integer_ssim_hip"));
    return NULL;
}

/* With the HIP backend active, model-driven dispatch resolves the `ssim`
 * feature to the HIP twin. Before the int64 kernel the twin was deliberately
 * unflagged and `ssim` fell back to the CPU (ADR-1154). Needs no device. */
static char *test_ssim_hip_dispatch(void)
{
    VmafFeatureExtractor *fex =
        vmaf_get_feature_extractor_by_feature_name("ssim", VMAF_FEATURE_EXTRACTOR_HIP);
    mu_assert("ssim must resolve to an extractor under the HIP flag", fex != NULL);
    mu_assert("ssim under the HIP flag must resolve to integer_ssim_hip",
              !strcmp(fex->name, "integer_ssim_hip"));
    mu_assert("integer_ssim_hip must carry VMAF_FEATURE_EXTRACTOR_HIP",
              (fex->flags & VMAF_FEATURE_EXTRACTOR_HIP) != 0);
    return NULL;
}

static char *test_ssim_cpu_hip_parity(void)
{
    double cpu[N_FRAMES] = {0.0};
    double gpu[N_FRAMES] = {0.0};
    int ran = 0;
    char *msg = run_cpu_ssim(cpu);
    if (msg)
        return msg;
    msg = run_hip_ssim(gpu, &ran);
    if (msg || !ran)
        return msg;
    double worst = 0.0;
    for (unsigned f = 0u; f < N_FRAMES; f++) {
        const double delta = fabs(cpu[f] - gpu[f]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr, "\nssim parity FAIL frame %u: cpu=%.17g hip=%.17g delta=%.3e\n",
                          f, cpu[f], gpu[f], delta);
        }
        worst = delta > worst ? delta : worst;
    }
    (void)fprintf(stderr, "[%ux%u %u bpc, %u frames, max delta %.3e] ", FIXTURE_W, FIXTURE_H,
                  FIXTURE_BPC, N_FRAMES, worst);
    mu_assert("ssim CPU vs. HIP delta exceeds places=4 tolerance (1e-4)", worst <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_ssim_hip_registered);
    mu_run_test(test_ssim_hip_dispatch);
    mu_run_test(test_ssim_cpu_hip_parity);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
