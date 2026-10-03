/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ssimulacra2 CPU vs. Metal: the twin returns the CPU's score bit for bit.
 *
 * The extractor is registered as `ssimulacra2` on the CPU
 * (core/src/feature/ssimulacra2.c) and as `ssimulacra2_metal` on Metal; both
 * emit the scalar `ssimulacra2`. The CUDA twin returns the CPU's score bit for
 * bit (ADR-1391 for the device pipeline, ADR-1433 for the sums of the
 * per-pixel SSIM and edge terms, which the CPU adds pixel after pixel into one
 * double), and test_cuda_ssimulacra2_parity.c asserts `==`; this test asserts
 * the same of the Metal twin: no tolerance, every frame, in every case.
 *
 * No state row records a Metal ssimulacra2 exactness defect: the test measures
 * whether the twin is exact. The cases follow the CUDA test: a distorted
 * 256x144 frame (five pyramid scales; the sixth, 8x5, is below the 8x8
 * floor), identical pictures (every sum is zero, the score is 100), and a
 * picture distorted in its lower third only (each sum starts with a run of
 * zeros). Added for Metal: 960x540 (all six scales), an odd frame size, 10-bit
 * input, and the refusal of 4:0:0 input, which has no chroma planes for the
 * colour conversion and which the CPU refuses with -EINVAL.
 *
 * The lifecycle case checks that the twin is submit/collect based; the CPU
 * extractor is not, so it means something only on a device.
 *
 * Skip behaviour: without a Metal device every comparison reports the skip
 * and the run exits 77. The registration case needs no device.
 */

#include "metal_twin.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "feature/feature_extractor.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static const char *const TWIN_NAME = METAL_TWIN("ssimulacra2_metal", "ssimulacra2");

#define NUM_FRAMES 3u

/* How the distorted picture differs from the reference. */
enum distortion {
    DISTORT_ALL = 0,     /* every pixel */
    DISTORT_NONE,        /* identical pictures */
    DISTORT_LOWER_THIRD, /* rows from two thirds down */
};

/* One comparison: a frame geometry, a bit depth and a distortion. */
typedef struct SsCase {
    const char *name;
    unsigned w;
    unsigned h;
    unsigned bpc;
    enum distortion mode;
} SsCase;

static bool row_is_distorted(enum distortion mode, unsigned row, unsigned rows)
{
    if (mode == DISTORT_NONE) {
        return false;
    }
    return mode == DISTORT_ALL || row >= rows - rows / 3u;
}

static void put_sample(VmafPicture *pic, unsigned plane, unsigned row, unsigned col, unsigned v)
{
    uint8_t *line = (uint8_t *)pic->data[plane] + ((size_t)row * (size_t)pic->stride[plane]);
    if (pic->bpc <= 8u) {
        line[col] = (uint8_t)(v & 0xFFu);
    } else {
        ((uint16_t *)line)[col] = (uint16_t)((v & 0xFFu) << (pic->bpc - 8u));
    }
}

/* Plane `p` of the reference (distorted == false) or of the distorted picture
 * of frame `frame`. Chroma gets deterministic non-128 values, so the colour
 * conversion and the score are non-trivial. */
static void fill_plane(VmafPicture *pic, const SsCase *c, unsigned p, unsigned frame,
                       bool distorted)
{
    for (unsigned row = 0; row < pic->h[p]; row++) {
        const bool noisy = distorted && row_is_distorted(c->mode, row, pic->h[p]);
        for (unsigned col = 0; col < pic->w[p]; col++) {
            unsigned v = (row + col + frame * 5u) & 0xFFu;
            if (p > 0u) {
                v = (row * 2u + col + p * 19u + frame) & 0xFFu;
            }
            if (noisy) {
                v += (p == 0u) ? ((row * 2u + col + frame * 3u) % 13u) :
                                 ((row + col * 3u + frame) % 7u);
            }
            put_sample(pic, p, row, col, v);
        }
    }
}

static int fill_picture(VmafPicture *pic, const SsCase *c, unsigned frame, bool distorted)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, c->bpc, c->w, c->h);
    if (err) {
        return err;
    }
    for (unsigned p = 0; p < 3u; p++) {
        fill_plane(pic, c, p, frame, distorted);
    }
    return 0;
}

static int feed_frames(VmafContext *vmaf, const SsCase *c)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_picture(&ref, c, i, false);
        if (err) {
            return err;
        }
        err = fill_picture(&dist, c, i, true);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            return err;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err) {
            return err;
        }
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

/* A context with the CPU `ssimulacra2`, or with the twin on `state`. */
static int ss_context(VmafContext **vmaf, void *state)
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    int err = vmaf_init(vmaf, cfg);
    if (!err && state) {
        err = metal_twin_import(*vmaf, state);
    }
    if (!err) {
        err = vmaf_use_feature(*vmaf, state ? TWIN_NAME : "ssimulacra2", NULL);
    }
    return err;
}

/* The score of every frame through one extractor; returns the first error. */
static int ss_scores(void *state, const SsCase *c, double out[NUM_FRAMES])
{
    VmafContext *vmaf = NULL;
    int err = ss_context(&vmaf, state);
    if (!err) {
        err = feed_frames(vmaf, c);
    }
    for (unsigned i = 0; i < NUM_FRAMES && !err; i++) {
        err = vmaf_feature_score_at_index(vmaf, "ssimulacra2", &out[i], i);
    }
    const int closed = vmaf ? vmaf_close(vmaf) : 0;
    return err ? err : closed;
}

/* The Metal state, or NULL with the reason printed when there is none. */
static void *metal_device(void)
{
    void *state = NULL;
    if (metal_twin_open(&state) != 0 || state == NULL) {
        (void)fprintf(stderr, "[skip: no Metal device] ");
        mu_skipped = 1;
        return NULL;
    }
    return state;
}

/* The frames of the case whose twin score is not the CPU's, each one
 * reported; UINT32_MAX when a run failed. A skipped twin leg counts as 0.
 * `cpu` returns the CPU's scores. */
static unsigned exact_mismatches(const SsCase *c, double cpu[NUM_FRAMES])
{
    double gpu[NUM_FRAMES] = {0.0};
    void *state = metal_device();
    if (!state) {
        return 0u;
    }
    const int gpu_err = ss_scores(state, c, gpu);
    (void)metal_twin_close(state);
    const int cpu_err = gpu_err ? 0 : ss_scores(NULL, c, cpu);
    if (gpu_err || cpu_err) {
        (void)fprintf(stderr, "\n%s: run failed (%s %d, cpu %d)\n", c->name, METAL_TWIN_BACKEND,
                      gpu_err, cpu_err);
        return UINT32_MAX;
    }
    unsigned mismatches = 0u;
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        if (isfinite(cpu[i]) && cpu[i] == gpu[i]) {
            continue;
        }
        mismatches++;
        (void)fprintf(stderr, "\nssimulacra2 %s %ux%u %u-bit frame %u: cpu=%.17g metal=%.17g\n",
                      c->name, c->w, c->h, c->bpc, i, cpu[i], gpu[i]);
    }
    return mismatches;
}

static char *check_case(const SsCase *c, char *message)
{
    double cpu[NUM_FRAMES] = {0.0};
    mu_assert(message, exact_mismatches(c, cpu) == 0u);
    return NULL;
}

static char *test_ssimulacra2_metal_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(TWIN_NAME);
    mu_assert("ssimulacra2_metal extractor must be registered", fex != NULL);
    mu_assert("ssimulacra2_metal name matches", !strcmp(fex->name, TWIN_NAME));
    return NULL;
}

/* Registration check of any design: the twin is a Metal extractor with an
 * init and a close, and provides the `ssimulacra2` feature the CPU does. */
static char *test_ssimulacra2_metal_lifecycle(void)
{
    if (metal_twin_device_only()) {
        return NULL;
    }
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(TWIN_NAME);
    mu_assert("ssimulacra2_metal extractor must be registered", fex != NULL);
    mu_assert("ssimulacra2_metal must provide init and close", fex->init && fex->close);
    mu_assert("ssimulacra2_metal must score through submit/collect or extract",
              fex->extract != NULL || (fex->submit != NULL && fex->collect != NULL));
    mu_assert("ssimulacra2_metal must be a Metal extractor",
              (fex->flags & VMAF_FEATURE_EXTRACTOR_METAL) != 0);
    return NULL;
}

/* The distorted pictures must score differently from frame to frame, or the
 * comparison cannot tell a stale score from a fresh one. */
static char *test_ssimulacra2_exact(void)
{
    static const SsCase c = {"distorted", 256u, 144u, 8u, DISTORT_ALL};
    double cpu[NUM_FRAMES] = {0.0};
    mu_assert("ssimulacra2_metal differs from the CPU extractor", exact_mismatches(&c, cpu) == 0u);
    mu_assert("ssimulacra2 fixture frames must score differently",
              mu_skipped || (cpu[0] != cpu[1] && cpu[1] != cpu[2]));
    return NULL;
}

/* Every term of every sum is zero. */
static char *test_ssimulacra2_identical_frames_exact(void)
{
    static const SsCase c = {"identical", 256u, 144u, 8u, DISTORT_NONE};
    double cpu[NUM_FRAMES] = {0.0};
    mu_assert("ssimulacra2_metal differs from the CPU extractor on identical frames",
              exact_mismatches(&c, cpu) == 0u);
    mu_assert("identical pictures must score 100", mu_skipped || cpu[0] == 100.0);
    return NULL;
}

/* The sums start with a run of zero terms and pick up in the last third. */
static char *test_ssimulacra2_lower_third_exact(void)
{
    static const SsCase c = {"lower third", 256u, 144u, 8u, DISTORT_LOWER_THIRD};
    return check_case(&c, "ssimulacra2_metal differs from the CPU with a distorted lower third");
}

/* 960x540 runs all six pyramid scales. */
static char *test_ssimulacra2_large_exact(void)
{
    static const SsCase c = {"large", 960u, 540u, 8u, DISTORT_ALL};
    return check_case(&c, "ssimulacra2_metal differs from the CPU extractor at 960x540");
}

/* Odd at every scale: 255x141 halves to 128x71, 64x36, 32x18 and 16x9. */
static char *test_ssimulacra2_odd_frame_exact(void)
{
    static const SsCase c = {"odd frame", 255u, 141u, 8u, DISTORT_ALL};
    return check_case(&c, "ssimulacra2_metal differs from the CPU extractor on an odd frame");
}

static char *test_ssimulacra2_10bit_exact(void)
{
    static const SsCase c = {"10-bit", 256u, 144u, 10u, DISTORT_ALL};
    return check_case(&c, "ssimulacra2_metal differs from the CPU extractor at 10 bits");
}

/* 4:0:0 has no chroma planes for the colour conversion; the CPU refuses it
 * with -EINVAL at init and the twin must too. */
static int alloc_monochrome(VmafPicture *pic)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV400P, 8u, 64u, 64u);
    if (!err) {
        /* Fill the luma plane: a twin that wrongly accepts must not read
         * uninitialised memory. */
        memset(pic->data[0], 100, (size_t)pic->stride[0] * (size_t)pic->h[0]);
    }
    return err;
}

static int monochrome_status(void *state, bool *setup_failed)
{
    VmafContext *vmaf = NULL;
    VmafPicture ref;
    VmafPicture dist;
    int err = ss_context(&vmaf, state);
    if (!err) {
        err = alloc_monochrome(&ref);
    }
    if (!err) {
        err = alloc_monochrome(&dist);
        if (err) {
            (void)vmaf_picture_unref(&ref);
        }
    }
    *setup_failed = err != 0;
    if (!err) {
        /* Only this call runs init(): its status is the twin's answer. */
        err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    }
    if (vmaf) {
        (void)vmaf_close(vmaf);
    }
    return err;
}

static char *test_ssimulacra2_rejects_monochrome(void)
{
    void *state = metal_device();
    if (!state) {
        return NULL;
    }
    bool setup_failed = false;
    const int rc = monochrome_status(state, &setup_failed);
    (void)metal_twin_close(state);
    mu_assert("4:0:0 request: context setup failed (not init's refusal)", !setup_failed);
    if (rc != -EINVAL) {
        (void)fprintf(stderr, "\n4:0:0 request returned %d\n", rc);
    }
    mu_assert("ssimulacra2_metal must reject 4:0:0 input with -EINVAL", rc == -EINVAL);
    return NULL;
}

static void run_exact_cases(void)
{
    metal_run_case(test_ssimulacra2_exact);
    metal_run_case(test_ssimulacra2_identical_frames_exact);
    metal_run_case(test_ssimulacra2_lower_third_exact);
    metal_run_case(test_ssimulacra2_large_exact);
    metal_run_case(test_ssimulacra2_odd_frame_exact);
    metal_run_case(test_ssimulacra2_10bit_exact);
}

char *run_tests(void)
{
    metal_run_case(test_ssimulacra2_metal_registered);
    metal_run_case(test_ssimulacra2_metal_lifecycle);
    run_exact_cases();
    metal_run_case(test_ssimulacra2_rejects_monochrome);
    return metal_first_failure;
}

/* NOLINTEND(modernize-use-nullptr) */
