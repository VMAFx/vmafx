/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * speed_temporal CPU vs. CUDA parity test (ADR-0965).
 *
 * Asserts that `Speed_temporal_feature_speed_temporal_score` from the CPU
 * extractor `speed_temporal` and the CUDA twin `speed_temporal_cuda` are
 * equal.
 *
 * Since ADR-1477 the comparison is an equality: speed.c evaluates Netflix's
 * fp64 statements (libvmaf/src/feature/speed.c of Netflix/vmaf 9e48141b:
 * `1.0 / sqrt(1 + t * t)` at 418 and 423, the `log2()` of update_entropy()
 * at 802 and of get_speed_score() at 897 to 928), and the twin reproduces
 * each of them, the rotation on the device (feature/speed_givens.h) and the
 * logarithms on the host with the CPU extractor's own C library
 * (speed_internal_gpu_tail_scores()).
 * The test allowed 1e-4 (places=4, ADR-0214) before.
 *
 * Skip behaviour: if `vmaf_cuda_state_init()` fails (no CUDA driver or no
 * device visible) the test emits `[skip: no CUDA device]` and exits 77, meson's
 * "skipped" status.
 * Mirrors the pattern from test_cuda_motion3_parity.c.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "float_bits.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this test mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

/* speed_temporal needs >=2 frames; same fixture geometry as the chroma test. */
#ifndef FIXTURE_W
#define FIXTURE_W 768u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 432u
#endif
#define FIXTURE_BPC 8u
#define NUM_FRAMES 3u

static int fill_fixture(VmafPicture *pic, unsigned frame_idx, int distort)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const unsigned base = (row * 3u + col * 5u + frame_idx * 11u) & 0xFFu;
            const unsigned d = distort ? ((row + col) & 0x07u) : 0u;
            y[row * pic->stride[0] + col] = (uint8_t)((base + d) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            (void)memset(plane + row * pic->stride[p], 128, pic->w[p]);
        }
    }
    return 0;
}

static char *feed_temporal_frames(VmafContext *vmaf)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_fixture(&ref, i, 0);
        mu_assert("fill_fixture(ref) failed", !err);
        err = fill_fixture(&dist, i, 1);
        mu_assert("fill_fixture(dist) failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("vmaf_read_pictures failed", !err);
    }
    int err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("vmaf_read_pictures(EOS) failed", !err);
    return NULL;
}

static char *setup_temporal_context(VmafContext **vmaf, const char *fex_name, int use_cuda,
                                    VmafCudaState *cu_state)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    int err = vmaf_init(vmaf, cfg);
    mu_assert("vmaf_init failed", !err);

    if (use_cuda) {
        err = vmaf_cuda_import_state(*vmaf, cu_state);
        mu_assert("vmaf_cuda_import_state failed", !err);
    }

    err = vmaf_use_feature(*vmaf, fex_name, NULL);
    mu_assert("vmaf_use_feature failed", !err);
    return NULL;
}

static char *drive(const char *fex_name, int use_cuda, double *out_score)
{
    *out_score = NAN;
    int err = 0;
    VmafCudaState *cu_state = NULL;
    if (use_cuda) {
        VmafCudaConfiguration cuda_cfg = {0};
        int rc = vmaf_cuda_state_init(&cu_state, cuda_cfg);
        if (rc != 0 || cu_state == NULL) {
            (void)fprintf(stderr, "[skip: no CUDA device] ");
            mu_skipped = 1; /* exit 77, not a pass */
            return NULL;
        }
    }

    VmafContext *vmaf = NULL;
    char *msg = setup_temporal_context(&vmaf, fex_name, use_cuda, cu_state);
    if (msg) {
        if (use_cuda)
            (void)vmaf_cuda_state_free(cu_state);
        return msg;
    }

    msg = feed_temporal_frames(vmaf);
    if (msg) {
        (void)vmaf_close(vmaf);
        if (use_cuda)
            (void)vmaf_cuda_state_free(cu_state);
        return msg;
    }

    err = vmaf_feature_score_at_index(vmaf, "Speed_temporal_feature_speed_temporal_score",
                                      out_score, 1u);
    mu_assert("vmaf_feature_score_at_index(speed_temporal, idx=1) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("vmaf_close failed", !err);

    if (use_cuda) {
        err = vmaf_cuda_state_free(cu_state);
        mu_assert("vmaf_cuda_state_free failed", !err);
    }
    return NULL;
}

static char *test_speed_temporal_cpu_cuda_parity(void)
{
    double cpu_score = NAN;
    double cuda_score = NAN;

    char *msg = drive("speed_temporal", 0, &cpu_score);
    if (msg)
        return msg;

    msg = drive("speed_temporal_cuda", 1, &cuda_score);
    if (msg)
        return msg;

    if (isnan(cuda_score))
        return NULL;

    if (!vmaf_test_identical_f64(cpu_score, cuda_score)) {
        (void)fprintf(stderr, "\nspeed_temporal parity FAIL: cpu=%.17g cuda=%.17g delta=%.2e\n",
                      cpu_score, cuda_score, fabs(cpu_score - cuda_score));
    }
    mu_assert("speed_temporal_cuda does not return the CPU extractor's score",
              vmaf_test_identical_f64(cpu_score, cuda_score));
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_speed_temporal_cpu_cuda_parity);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
