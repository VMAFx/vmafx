/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * SYCL float_ssim CPU vs. SYCL parity test (Research-0985, ADR-1370).
 *
 * `float_ssim_sycl` (vmaf_fex_float_ssim_sycl in integer_ssim_sycl.cpp)
 * decimates on the device exactly as ssim.c does (bit-identical planes),
 * then runs the separable 11-tap Gaussian + per-WG float partial sums.
 *
 * Precision contract: the CPU path (float_ssim.c via iqa_ssim) uses the
 * L×C×S decomposition with a sqrt(var_ref * var_cmp) contrast term, while
 * the SYCL path uses the combined formula 2*covar / (var_ref + var_cmp).
 * These are mathematically distinct — the places=3 (5e-04) tolerance here
 * accommodates both the formula difference and Arc A380 class fp32
 * accumulation drift. See Research-0985 §3.
 *
 * Coverage (every case scores the same frames on both sides, per frame):
 *   positive  the FIXTURE_W x FIXTURE_H auto case (320x180 = scale 1; the
 *             meson `_large` variant is 960x540 = auto scale 2), auto scale
 *             2 on an odd width (853x480), explicit scales 2, 3, 5 and 10,
 *             an odd scale on odd dimensions, 10- and 12-bit samples, and
 *             enable_lcs at scale 3 (float_ssim_l / _c / _s too);
 *   boundary  a plane decimated to exactly the 11x11 Gaussian (110x110 at
 *             scale 10), and 3840x2160 auto (scale 8) on the twin gate;
 *   negative  a plane decimated below 11x11 (100x100 at scale 10): the
 *             twin gate refuses it and a direct float_ssim_sycl run fails.
 *
 * Skip behaviour: if vmaf_sycl_state_init() fails (no oneAPI runtime or
 * no device visible) the device cases emit "[skip: no SYCL device]" and
 * pass, mirroring test_sycl_motion3_parity.c.
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
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* 320×180 stays at auto scale 1 (180/256 ≈ 0.7); the `_large` meson variant
 * rebuilds this file at 960x540 (auto scale 2, ADR-1206). */
#ifndef FIXTURE_W
#define FIXTURE_W 320u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 180u
#endif

#define PARITY_TOL 5e-04
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
                ((uint16_t *)line)[col] = (uint16_t)v;
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
        if (err)
            return err;
        err = fill_pic(&dist, pc, i + 1u);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            return err;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err)
            return err;
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

static VmafFeatureDictionary *case_options(const ParityCase *pc)
{
    VmafFeatureDictionary *opts = NULL;
    if (pc->scale && vmaf_feature_dictionary_set(&opts, "scale", pc->scale))
        return NULL;
    if (pc->enable_lcs && vmaf_feature_dictionary_set(&opts, "enable_lcs", "true"))
        return NULL;
    return opts;
}

/* One SYCL state per context, as test_sycl_twin_option_parity.c does. */
static VmafSyclState *open_device(void)
{
    VmafSyclState *sycl = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    if (vmaf_sycl_state_init(&sycl, sycl_cfg) != 0)
        return NULL;
    return sycl;
}

static bool device_present(void)
{
    VmafSyclState *sycl = open_device();
    if (!sycl) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return false;
    }
    vmaf_sycl_state_free(&sycl);
    return true;
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

/* Runs `extractor` over the case, on a fresh SYCL state when `on_device`;
 * scores[frame][k] follow score_names. *err receives the
 * vmaf_read_pictures() status instead of failing, so the negative case can
 * assert it. */
static char *run_case(bool on_device, const char *extractor, const ParityCase *pc,
                      double scores[N_FRAMES][N_SCORES], int *err)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    VmafSyclState *sycl = on_device ? open_device() : NULL;
    mu_assert("SYCL state init failed", sycl || !on_device);
    mu_assert("vmaf_init failed", !vmaf_init(&vmaf, cfg));
    if (sycl)
        mu_assert("vmaf_sycl_import_state failed", !vmaf_sycl_import_state(vmaf, sycl));
    mu_assert("vmaf_use_feature failed", !vmaf_use_feature(vmaf, extractor, case_options(pc)));
    *err = feed_frames(vmaf, pc);
    char *msg = *err ? NULL : collect_scores(vmaf, pc, scores);
    mu_assert("vmaf_close failed", !vmaf_close(vmaf));
    if (sycl)
        vmaf_sycl_state_free(&sycl);
    return msg;
}

static char *compare_case(const ParityCase *pc)
{
    double cpu[N_FRAMES][N_SCORES] = {{0.0}};
    double gpu[N_FRAMES][N_SCORES] = {{0.0}};
    int err = 0;
    char *msg = run_case(false, "float_ssim", pc, cpu, &err);
    if (msg)
        return msg;
    mu_assert("CPU float_ssim run failed", !err);
    msg = run_case(true, "float_ssim_sycl", pc, gpu, &err);
    if (msg)
        return msg;
    mu_assert("SYCL float_ssim_sycl run failed", !err);
    const unsigned n_scores = pc->enable_lcs ? N_SCORES : 1u;
    for (unsigned i = 0; i < N_FRAMES; i++) {
        for (unsigned k = 0; k < n_scores; k++) {
            const double delta = fabs(cpu[i][k] - gpu[i][k]);
            if (delta > PARITY_TOL) {
                (void)fprintf(stderr, "\n%s %ux%u %u-bit scale=%s frame %u: cpu=%.8f sycl=%.8f\n",
                              score_names[k], pc->w, pc->h, pc->bpc, pc->scale ? pc->scale : "auto",
                              i, cpu[i][k], gpu[i][k]);
            }
            mu_assert("float_ssim CPU vs. SYCL delta exceeds places=3 (5e-04)",
                      delta <= PARITY_TOL);
        }
    }
    return NULL;
}

static char *test_float_ssim_sycl_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ssim_sycl");
    mu_assert("float_ssim_sycl extractor must be registered", fex != NULL);
    mu_assert("float_ssim_sycl name matches", !strcmp(fex->name, "float_ssim_sycl"));
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

static char *test_float_ssim_cpu_sycl_parity(void)
{
    if (!device_present())
        return NULL;
    char *msg = NULL;
    for (size_t i = 0; i < sizeof(parity_cases) / sizeof(parity_cases[0]) && !msg; i++)
        msg = compare_case(&parity_cases[i]);
    return msg;
}

/* Gate verdict of the SYCL twin for a CPU float_ssim request (ADR-1324). */
static int twin_verdict(unsigned w, unsigned h, const char *scale, const char **twin)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    VmafSyclState *sycl = open_device();
    if (!sycl)
        return -ENODEV;
    if (vmaf_init(&vmaf, cfg)) {
        vmaf_sycl_state_free(&sycl);
        return -ENOMEM;
    }
    int err = vmaf_sycl_import_state(vmaf, sycl);
    const ParityCase pc = CASE(w, h, 8u, scale, false);
    VmafFeatureDictionary *opts = case_options(&pc);
    const VmafPictureConfiguration pic_cfg = {
        .pic_params = {.w = w, .h = h, .bpc = 8u, .pix_fmt = VMAF_PIX_FMT_YUV420P}};
    if (!err)
        err = vmaf_feature_backend_twin(vmaf, "float_ssim", opts, &pic_cfg, twin, NULL);
    (void)vmaf_feature_dictionary_free(&opts);
    (void)vmaf_close(vmaf);
    vmaf_sycl_state_free(&sycl);
    return err;
}

static char *test_float_ssim_sycl_gate(void)
{
    if (!device_present())
        return NULL;
    const char *twin = NULL;
    const int uhd = twin_verdict(3840u, 2160u, NULL, &twin);
    const char *tiny_twin = NULL;
    const int tiny = twin_verdict(100u, 100u, "10", &tiny_twin);
    double scores[N_FRAMES][N_SCORES] = {{0.0}};
    int err = 0;
    const ParityCase below = CASE(100u, 100u, 8u, "10", false);
    char *msg = run_case(true, "float_ssim_sycl", &below, scores, &err);
    if (msg)
        return msg;
    mu_assert("the twin must serve 3840x2160 at auto scale 8", uhd == 0);
    mu_assert("the twin name is float_ssim_sycl", twin && !strcmp(twin, "float_ssim_sycl"));
    mu_assert("a plane decimated below 11x11 must fall back", tiny == -ENOTSUP);
    mu_assert("a direct run below 11x11 must fail", err != 0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_ssim_sycl_registered);
    mu_run_test(test_float_ssim_cpu_sycl_parity);
    mu_run_test(test_float_ssim_sycl_gate);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
