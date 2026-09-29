/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Shared SYCL frame planes (ADR-1369).
 *
 * psnr_sycl, psnr_hvs_sycl and motion_v2_sycl read the frame the SYCL state
 * already holds: luma from the shared frame, Cb / Cr from the opt-in shared
 * chroma planes, uploaded once per frame for every twin. This gate runs the
 * three twins together in one context over several frames whose chroma
 * changes every frame and differs between reference and distorted, at an odd
 * width so the chroma rows are pitched (the 2-D copy path), and compares each
 * frame with the CPU extractors. A stale, swapped or twice-uploaded plane
 * shows up as a chroma score mismatch.
 *
 * The API cases pin the contract of the helpers themselves: geometry and
 * ordering errors, once-per-frame upload, and the double-buffered slots.
 *
 * Skip behaviour: without a SYCL device the test prints
 * "[skip: no SYCL device]" and passes, like test_sycl_psnr_hvs_parity.c.
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
#include "sycl/common.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Odd luma width: 4:2:0 chroma is 125 samples wide, below its row stride. */
#define FIXTURE_W 250u
#define FIXTURE_H 138u
#define FIXTURE_FRAMES 4u
#define HVS_TOL 1e-4
#define EXACT_TOL 1e-9
#define API_W 32u
#define API_H 32u

static const char *const g_features[] = {"psnr_y",
                                         "psnr_cb",
                                         "psnr_cr",
                                         "psnr_hvs",
                                         "psnr_hvs_cb",
                                         "psnr_hvs_cr",
                                         "VMAF_integer_feature_motion_v2_sad_score"};
#define N_FEATURES (sizeof(g_features) / sizeof(g_features[0]))

static uint8_t pattern(unsigned plane, unsigned row, unsigned col, unsigned frame, unsigned salt)
{
    const unsigned mix = (row * (3u + plane)) ^ (col * (5u + salt)) ^ (frame * 37u + plane * 11u);
    return (uint8_t)((mix + (salt * 29u) + (frame * 13u)) & 0xFFu);
}

static int fill_pic(VmafPicture *pic, enum VmafPixelFormat fmt, unsigned w, unsigned h,
                    unsigned frame, unsigned salt)
{
    const int err = vmaf_picture_alloc(pic, fmt, 8u, w, h);
    if (err)
        return err;
    for (unsigned p = 0; p < 3u; p++) {
        uint8_t *data = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                data[(size_t)row * (size_t)pic->stride[p] + col] =
                    pattern(p, row, col, frame, salt);
            }
        }
    }
    return 0;
}

static int feed_frames(VmafContext *vmaf)
{
    for (unsigned frame = 0; frame < FIXTURE_FRAMES; frame++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_pic(&ref, VMAF_PIX_FMT_YUV420P, FIXTURE_W, FIXTURE_H, frame, 0u);
        if (err)
            return err;
        err = fill_pic(&dist, VMAF_PIX_FMT_YUV420P, FIXTURE_W, FIXTURE_H, frame, 1u);
        if (err)
            return err | vmaf_picture_unref(&ref);
        err = vmaf_read_pictures(vmaf, &ref, &dist, frame);
        if (err)
            return err;
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

/* A context with psnr, psnr_hvs and motion_v2: the CPU extractors, or their
 * SYCL twins when `sycl_state` is set. */
static char *open_features(VmafSyclState *sycl_state, VmafContext **vmaf)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    mu_assert("vmaf_init failed", !vmaf_init(vmaf, cfg));
    if (sycl_state)
        mu_assert("vmaf_sycl_import_state failed", !vmaf_sycl_import_state(*vmaf, sycl_state));
    const char *const names[3] = {sycl_state ? "psnr_sycl" : "psnr",
                                  sycl_state ? "psnr_hvs_sycl" : "psnr_hvs",
                                  sycl_state ? "motion_v2_sycl" : "motion_v2"};
    for (unsigned i = 0; i < 3u; i++)
        mu_assert("vmaf_use_feature failed", !vmaf_use_feature(*vmaf, names[i], NULL));
    return NULL;
}

/* Runs the fixture through open_features() and stores every frame's scores. */
static char *run_features(VmafSyclState *sycl_state, double scores[N_FEATURES][FIXTURE_FRAMES])
{
    VmafContext *vmaf = NULL;
    mu_assert_msg(open_features(sycl_state, &vmaf));
    mu_assert("feeding the fixture failed", !feed_frames(vmaf));
    for (unsigned f = 0; f < N_FEATURES; f++) {
        for (unsigned frame = 0; frame < FIXTURE_FRAMES; frame++) {
            mu_assert("score missing",
                      !vmaf_feature_score_at_index(vmaf, g_features[f], &scores[f][frame], frame));
        }
    }
    mu_assert("vmaf_close failed", !vmaf_close(vmaf));
    return NULL;
}

static VmafSyclState *open_state(void)
{
    VmafSyclState *state = NULL;
    VmafSyclConfiguration cfg = {.device_index = -1};
    if (vmaf_sycl_state_init(&state, cfg) != 0 || !state) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        mu_skipped = 1;
        return NULL;
    }
    return state;
}

static char *test_twins_share_planes(void)
{
    static double cpu[N_FEATURES][FIXTURE_FRAMES];
    static double gpu[N_FEATURES][FIXTURE_FRAMES];
    char *msg = run_features(NULL, cpu);
    if (msg)
        return msg;
    VmafSyclState *state = open_state();
    if (!state)
        return NULL;
    msg = run_features(state, gpu);
    vmaf_sycl_state_free(&state);
    if (msg)
        return msg;
    for (unsigned f = 0; f < N_FEATURES; f++) {
        const double tol = strstr(g_features[f], "hvs") ? HVS_TOL : EXACT_TOL;
        for (unsigned frame = 0; frame < FIXTURE_FRAMES; frame++) {
            const double delta = fabs(cpu[f][frame] - gpu[f][frame]);
            if (delta > tol) {
                (void)fprintf(stderr, "\n%s frame %u: cpu=%.10f sycl=%.10f delta=%.2e\n",
                              g_features[f], frame, cpu[f][frame], gpu[f][frame], delta);
            }
            mu_assert("SYCL twin sharing the uploaded planes differs from the CPU", delta <= tol);
        }
    }
    return NULL;
}

/* The shared Cr plane of the compute slot must hold `frame`'s distorted Cr. */
static char *check_shared_cr(VmafSyclState *state, unsigned frame)
{
    uint8_t got[(API_W / 2u) * (API_H / 2u)];
    mu_assert("readback", !vmaf_sycl_memcpy_d2h(state, got, vmaf_sycl_get_shared_plane(state, 0, 2),
                                                sizeof(got)));
    for (unsigned row = 0; row < API_H / 2u; row++) {
        for (unsigned col = 0; col < API_W / 2u; col++) {
            mu_assert("shared Cr plane must hold the distorted picture's Cr samples",
                      got[row * (API_W / 2u) + col] == pattern(2u, row, col, frame, 1u));
        }
    }
    return NULL;
}

/* A second chroma upload of the same frame, with other content, is a no-op. */
static char *check_upload_once(VmafSyclState *state, enum VmafPixelFormat fmt, unsigned frame)
{
    VmafPicture other;
    mu_assert("other alloc", !fill_pic(&other, fmt, API_W, API_H, frame + 7u, 3u));
    mu_assert("second chroma upload of a frame is a no-op",
              !vmaf_sycl_shared_chroma_upload(state, &other, &other));
    mu_assert("unref", !vmaf_picture_unref(&other));
    return check_shared_cr(state, frame);
}

static char *upload_frame(VmafSyclState *state, enum VmafPixelFormat fmt, unsigned frame)
{
    VmafPicture ref;
    VmafPicture dis;
    mu_assert("ref alloc", !fill_pic(&ref, fmt, API_W, API_H, frame, 0u));
    mu_assert("dis alloc", !fill_pic(&dis, fmt, API_W, API_H, frame, 1u));
    mu_assert("luma upload", !vmaf_sycl_shared_frame_upload(state, &ref, &dis));
    mu_assert("chroma upload of a matching picture must succeed",
              !vmaf_sycl_shared_chroma_upload(state, &ref, &dis));
    mu_assert("upload wait", !vmaf_sycl_wait_last_upload(state));
    mu_assert("unref", !vmaf_picture_unref(&ref) && !vmaf_picture_unref(&dis));
    mu_assert_msg(check_shared_cr(state, frame));
    return check_upload_once(state, fmt, frame);
}

static char *check_init_rejections(VmafSyclState *state)
{
    mu_assert("chroma before luma must be rejected",
              vmaf_sycl_shared_chroma_init(state, API_W / 2u, API_H / 2u) == -EINVAL);
    mu_assert("luma init", !vmaf_sycl_shared_frame_init(state, API_W, API_H, 8u));
    mu_assert("zero chroma width must be rejected",
              vmaf_sycl_shared_chroma_init(state, 0u, API_H / 2u) == -EINVAL);
    mu_assert("chroma init", !vmaf_sycl_shared_chroma_init(state, API_W / 2u, API_H / 2u));
    mu_assert("chroma init is idempotent",
              !vmaf_sycl_shared_chroma_init(state, API_W / 2u, API_H / 2u));
    mu_assert("a second geometry must be rejected",
              vmaf_sycl_shared_chroma_init(state, API_W, API_H) == -EINVAL);
    return NULL;
}

static char *check_rejections(VmafSyclState *state)
{
    mu_assert_msg(check_init_rejections(state));
    mu_assert("plane 3 does not exist", vmaf_sycl_get_shared_plane(state, 1, 3u) == NULL);
    VmafPicture pic;
    mu_assert("alloc", !fill_pic(&pic, VMAF_PIX_FMT_YUV420P, API_W, API_H, 0u, 0u));
    mu_assert("chroma upload before any luma upload must be rejected",
              vmaf_sycl_shared_chroma_upload(state, &pic, &pic) == -EINVAL);
    mu_assert("missing picture must be rejected",
              vmaf_sycl_shared_chroma_upload(state, NULL, &pic) == -EINVAL);
    mu_assert("unref", !vmaf_picture_unref(&pic));
    return NULL;
}

static char *test_shared_chroma_api(void)
{
    VmafSyclState *state = open_state();
    if (!state)
        return NULL;
    char *msg = check_rejections(state);
    void *slot_a = NULL;
    if (!msg) {
        msg = upload_frame(state, VMAF_PIX_FMT_YUV420P, 0u);
        slot_a = vmaf_sycl_get_shared_plane(state, 1, 1u);
    }
    if (!msg)
        msg = upload_frame(state, VMAF_PIX_FMT_YUV420P, 1u);
    if (!msg && vmaf_sycl_get_shared_plane(state, 1, 1u) == slot_a)
        msg = "consecutive frames must land in different slots";
    if (!msg) {
        VmafPicture wrong;
        mu_assert("alloc", !fill_pic(&wrong, VMAF_PIX_FMT_YUV444P, API_W, API_H, 2u, 0u));
        mu_assert("luma upload", !vmaf_sycl_shared_frame_upload(state, &wrong, &wrong));
        if (vmaf_sycl_shared_chroma_upload(state, &wrong, &wrong) != -EINVAL)
            msg = "a picture with other chroma geometry must be rejected";
        mu_assert("upload wait", !vmaf_sycl_wait_last_upload(state));
        mu_assert("unref", !vmaf_picture_unref(&wrong));
    }
    vmaf_sycl_state_free(&state);
    return msg;
}

char *run_tests(void)
{
    mu_run_test(test_shared_chroma_api);
    mu_run_test(test_twins_share_planes);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
