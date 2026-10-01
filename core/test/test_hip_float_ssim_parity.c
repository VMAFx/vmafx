/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_ssim CPU vs. HIP parity test (ADR-0958 round 4, ADR-1405).
 *
 * `float_ssim_hip` decimates on the device exactly as ssim.c does
 * (bit-identical planes, test_hip_float_ssim_decimate.c), then runs the
 * separable 11-tap Gaussian and forms each pixel's term as the CPU does
 * (l * c * s in double, ADR-1382). What is left against the CPU is the
 * rounding of the fp32 Gaussian sums, so the tolerance is the cross-backend
 * gate's 5e-5 (`float_ssim` in scripts/ci/cross_backend_parity_gate.py).
 *
 * Coverage (every case scores the same frames on both sides, per frame):
 *   positive  the FIXTURE_W x FIXTURE_H auto case (256x144 = scale 1; the
 *             meson `_large` variant is 960x540 = auto scale 2), auto scale
 *             2 on an odd width (853x480), explicit scales 2, 3, 5 and 10,
 *             an odd scale on odd dimensions, 10- and 12-bit samples, and
 *             enable_lcs at scale 3 (float_ssim_l / _c / _s too);
 *   boundary  a plane decimated to exactly the 11x11 Gaussian (110x110 at
 *             scale 10), and 3840x2160 auto (scale 8) on the twin gate;
 *   negative  a plane decimated below 11x11 (100x100 at scale 10): the
 *             twin gate refuses it and a direct float_ssim_hip run fails.
 *
 * The device cases skip when vmaf_hip_state_init() fails (no AMD GPU, no HIP
 * runtime) or when the twin is a scaffold (-ENOSYS without enable_hipcc).
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* 256x144 stays at auto scale 1 (144 / 256 = 0.6); the `_large` meson variant
 * rebuilds this file at 960x540 (auto scale 2, ADR-1206). */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif

#define PARITY_TOL 5e-05
#define N_FRAMES 3u
#define N_SCORES 4u

typedef struct ParityCase {
    const char *scale; /* NULL = auto */
    unsigned w;
    unsigned h;
    unsigned bpc;
    bool enable_lcs;
} ParityCase;

static const char *const score_names[N_SCORES] = {"float_ssim", "float_ssim_l", "float_ssim_c",
                                                  "float_ssim_s"};

/* Deterministic texture plus hashed noise on the distorted side, in the
 * picture's sample range. */
static unsigned sample_at(unsigned row, unsigned col, unsigned bpc, unsigned salt)
{
    const unsigned max = (1u << bpc) - 1u;
    const unsigned base = (((row ^ col) * 3u + row / 3u) << (bpc - 8u)) & max;
    if (!salt) {
        return base;
    }
    const unsigned hash = (row * 2654435761u) ^ (col * 40503u) ^ (salt * 97u);
    const int noise = (int)((hash >> 7) % 33u) - 16;
    const int value = (int)base + noise * (int)(1u << (bpc - 8u));
    return value < 0 ? 0u : ((unsigned)value > max ? max : (unsigned)value);
}

static void fill_plane(VmafPicture *pic, unsigned p, const ParityCase *pc, unsigned salt)
{
    for (unsigned row = 0; row < pic->h[p]; row++) {
        uint8_t *line = (uint8_t *)pic->data[p] + (size_t)row * pic->stride[p];
        for (unsigned col = 0; col < pic->w[p]; col++) {
            const unsigned v = p ? (128u << (pc->bpc - 8u)) : sample_at(row, col, pc->bpc, salt);
            if (pc->bpc > 8u) {
                const uint16_t sample = (uint16_t)v;
                memcpy(line + (2u * (size_t)col), &sample, sizeof(sample));
            } else {
                line[col] = (uint8_t)v;
            }
        }
    }
}

static int fill_pic(VmafPicture *pic, const ParityCase *pc, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, pc->bpc, pc->w, pc->h);
    if (err) {
        return err;
    }
    for (unsigned p = 0; p < 3u; p++) {
        fill_plane(pic, p, pc, salt);
    }
    return 0;
}

static int feed_frames(VmafContext *vmaf, const ParityCase *pc)
{
    for (unsigned i = 0; i < N_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_pic(&ref, pc, 0u);
        if (err) {
            return err;
        }
        err = fill_pic(&dist, pc, i + 1u);
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

static VmafFeatureDictionary *case_options(const ParityCase *pc)
{
    VmafFeatureDictionary *opts = NULL;
    if (pc->scale && vmaf_feature_dictionary_set(&opts, "scale", pc->scale)) {
        return NULL;
    }
    if (pc->enable_lcs && vmaf_feature_dictionary_set(&opts, "enable_lcs", "true")) {
        return NULL;
    }
    return opts;
}

/* One HIP state per context, as test_hip_twin_option_parity.c does. */
static VmafHipState *open_device(void)
{
    VmafHipState *hip = NULL;
    VmafHipConfiguration hip_cfg = {.device_index = -1};
    if (vmaf_hip_state_init(&hip, hip_cfg) != 0) {
        return NULL;
    }
    return hip;
}

static char *collect_scores(VmafContext *vmaf, const ParityCase *pc,
                            double scores[N_FRAMES][N_SCORES])
{
    const unsigned n_scores = pc->enable_lcs ? N_SCORES : 1u;
    for (unsigned i = 0; i < N_FRAMES; i++) {
        for (unsigned k = 0; k < n_scores; k++) {
            mu_assert("score missing",
                      !vmaf_feature_score_at_index(vmaf, score_names[k], &scores[i][k], i));
        }
    }
    return NULL;
}

/* Runs `extractor` over the case, on a fresh HIP state when `on_device`;
 * scores[frame][k] follow score_names. *err receives the
 * vmaf_read_pictures() status instead of failing, so the negative case can
 * assert it and a scaffold build can skip on -ENOSYS. */
static char *run_case(bool on_device, const char *extractor, const ParityCase *pc,
                      double scores[N_FRAMES][N_SCORES], int *err)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    VmafHipState *hip = on_device ? open_device() : NULL;
    mu_assert("HIP state init failed", hip || !on_device);
    mu_assert("vmaf_init failed", !vmaf_init(&vmaf, cfg));
    if (hip) {
        mu_assert("vmaf_hip_import_state failed", !vmaf_hip_import_state(vmaf, hip));
    }
    *err = vmaf_use_feature(vmaf, extractor, case_options(pc));
    if (!*err) {
        *err = feed_frames(vmaf, pc);
    }
    char *msg = *err ? NULL : collect_scores(vmaf, pc, scores);
    mu_assert("vmaf_close failed", !vmaf_close(vmaf));
    if (hip) {
        vmaf_hip_state_free(&hip);
    }
    return msg;
}

/* Whether the twin can run here: a device, and kernels built with hipcc.
 * Prints the skip reason otherwise. */
static bool twin_runs(void)
{
    VmafHipState *hip = open_device();
    if (!hip) {
        (void)fprintf(stderr, "[skip: no HIP device] ");
        return false;
    }
    vmaf_hip_state_free(&hip);
    const ParityCase probe = {NULL, 64u, 64u, 8u, false};
    double scores[N_FRAMES][N_SCORES] = {{0.0}};
    int err = 0;
    /* Only the scaffold's -ENOSYS skips; any other failure shows up in the
     * cases that follow. */
    (void)run_case(true, "float_ssim_hip", &probe, scores, &err);
    if (err == -ENOSYS) {
        (void)fprintf(stderr, "[skip: HIP scaffold ENOSYS] ");
        return false;
    }
    return true;
}

static char *compare_case(const ParityCase *pc)
{
    double cpu[N_FRAMES][N_SCORES] = {{0.0}};
    double gpu[N_FRAMES][N_SCORES] = {{0.0}};
    int err = 0;
    char *msg = run_case(false, "float_ssim", pc, cpu, &err);
    if (msg) {
        return msg;
    }
    mu_assert("CPU float_ssim run failed", !err);
    msg = run_case(true, "float_ssim_hip", pc, gpu, &err);
    if (msg) {
        return msg;
    }
    mu_assert("HIP float_ssim_hip run failed", !err);
    const unsigned n_scores = pc->enable_lcs ? N_SCORES : 1u;
    for (unsigned i = 0; i < N_FRAMES; i++) {
        for (unsigned k = 0; k < n_scores; k++) {
            const double delta = fabs(cpu[i][k] - gpu[i][k]);
            if (delta > PARITY_TOL) {
                (void)fprintf(stderr, "\n%s %ux%u %u-bit scale=%s frame %u: cpu=%.9f hip=%.9f\n",
                              score_names[k], pc->w, pc->h, pc->bpc, pc->scale ? pc->scale : "auto",
                              i, cpu[i][k], gpu[i][k]);
            }
            mu_assert("float_ssim CPU vs. HIP delta exceeds the parity gate's 5e-5",
                      delta <= PARITY_TOL);
        }
    }
    return NULL;
}

static char *test_float_ssim_hip_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ssim_hip");
    mu_assert("float_ssim_hip extractor must be registered", fex != NULL);
    mu_assert("float_ssim_hip name matches", !strcmp(fex->name, "float_ssim_hip"));
    return NULL;
}

#define CASE(width, height, depth, scale_opt, lcs)                                                 \
    {.scale = (scale_opt), .w = (width), .h = (height), .bpc = (depth), .enable_lcs = (lcs)}

static const ParityCase parity_cases[] = {
    CASE(FIXTURE_W, FIXTURE_H, 8u, NULL, false), /* auto: 1, or 2 in the _large build */
    CASE(853u, 480u, 8u, NULL, false),           /* auto 2, odd width */
    CASE(320u, 180u, 8u, "2", false),
    CASE(320u, 180u, 8u, "3", false),
    CASE(321u, 181u, 8u, "5", false), /* odd scale, odd dimensions */
    CASE(320u, 180u, 8u, "10", false),
    CASE(110u, 110u, 8u, "10", false), /* boundary: decimates to exactly 11x11 */
    CASE(400u, 224u, 10u, "3", false),
    CASE(400u, 224u, 12u, "2", false),
    CASE(320u, 180u, 8u, "3", true), /* enable_lcs on a decimated plane */
};

static char *test_float_ssim_cpu_hip_parity(void)
{
    if (!twin_runs()) {
        return NULL;
    }
    char *msg = NULL;
    for (size_t i = 0; i < sizeof(parity_cases) / sizeof(parity_cases[0]) && !msg; i++) {
        msg = compare_case(&parity_cases[i]);
    }
    return msg;
}

/* Gate verdict of the HIP twin for a CPU float_ssim request (ADR-1324). */
static int twin_verdict(unsigned w, unsigned h, const char *scale, const char **twin)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    VmafHipState *hip = open_device();
    if (!hip) {
        return -ENODEV;
    }
    if (vmaf_init(&vmaf, cfg)) {
        vmaf_hip_state_free(&hip);
        return -ENOMEM;
    }
    int err = vmaf_hip_import_state(vmaf, hip);
    const ParityCase pc = CASE(w, h, 8u, scale, false);
    VmafFeatureDictionary *opts = case_options(&pc);
    const VmafPictureConfiguration pic_cfg = {
        .pic_params = {.w = w, .h = h, .bpc = 8u, .pix_fmt = VMAF_PIX_FMT_YUV420P}};
    if (!err) {
        err = vmaf_feature_backend_twin(vmaf, "float_ssim", opts, &pic_cfg, twin, NULL);
    }
    (void)vmaf_feature_dictionary_free(&opts);
    (void)vmaf_close(vmaf);
    vmaf_hip_state_free(&hip);
    return err;
}

static char *test_float_ssim_hip_gate(void)
{
    if (!twin_runs()) {
        return NULL;
    }
    const char *twin = NULL;
    const int uhd = twin_verdict(3840u, 2160u, NULL, &twin);
    const char *tiny_twin = NULL;
    const int tiny = twin_verdict(100u, 100u, "10", &tiny_twin);
    double scores[N_FRAMES][N_SCORES] = {{0.0}};
    int err = 0;
    const ParityCase below = CASE(100u, 100u, 8u, "10", false);
    char *msg = run_case(true, "float_ssim_hip", &below, scores, &err);
    if (msg) {
        return msg;
    }
    mu_assert("the twin must serve 3840x2160 at auto scale 8", uhd == 0);
    mu_assert("the twin name is float_ssim_hip", twin && !strcmp(twin, "float_ssim_hip"));
    mu_assert("a plane decimated below 11x11 must fall back", tiny == -ENOTSUP);
    mu_assert("a direct run below 11x11 must fail with -EINVAL", err == -EINVAL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_ssim_hip_registered);
    mu_run_test(test_float_ssim_cpu_hip_parity);
    mu_run_test(test_float_ssim_hip_gate);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
