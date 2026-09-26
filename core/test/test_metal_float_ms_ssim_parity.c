/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Metal kernel coverage round 3 — float_ms_ssim CPU vs. Metal parity
 * (T8-2a; ADR-0589 metal-ssim-lcs-db-parity; ADR-0214 cross-backend gate;
 *  ADR-1334 Metal option parity; T-GAP-METAL-MS-SSIM-DB-CHROMA-OPTIONS-2026-09-07).
 *
 * `float_ms_ssim_metal` emits the aggregate `float_ms_ssim` score: a 5-scale
 * MS-SSIM pyramid combining luminance, contrast, and structure across the
 * Wang et al. weight vector (inheriting the baseline parity target from ADR-0589).
 * When enable_chroma is set, it also emits `float_ms_ssim_cb` and `float_ms_ssim_cr`.
 * When enable_db and clip_db are set, scores are converted to dB with
 * geometry-derived max_db ceiling.
 *
 * Fixture dims: the 5-scale 11-tap MS-SSIM pyramid requires every input
 * dimension to satisfy `min(w, h) >= GAUSSIAN_LEN << (SCALES - 1) = 11 << 4
 * = 176` — anything smaller is rejected at init with -EINVAL (see
 * `core/src/feature/float_ms_ssim.c:131-138`). For 4:2:0 chroma, dimensions
 * are halved, so chroma also requires >= 176. Use 512x384 so chroma is
 * 256x192, safely satisfying the pyramid minimum across all planes.
 *
 * Tolerance rationale: SSIM-family metrics are normalised to [0, 1] and
 * the Metal pyramid uses workgroup-partial-sum reductions across 5 scales,
 * each with separable convolution + downsample passes. ADR-0589 §"Test
 * plan" cites 1e-3 as the working parity target for the float_ssim /
 * float_ms_ssim family before bit-exact L/C/S separation lands; this test
 * inherits the same bound rather than the tighter 1e-4 from ADR-0214.
 *
 * Skip behaviour: -ENODEV from `vmaf_metal_state_init` -> clean skip on
 * Linux / Windows / Intel Mac.
 *
 * Cross-references:
 *   - core/src/feature/metal/float_ms_ssim_metal.mm
 *   - core/src/feature/float_ms_ssim.c
 *   - core/test/test_metal_float_ssim_parity.c (sibling, single-scale SSIM)
 *   - docs/adr/0589-metal-ssim-lcs-db-parity.md
 *   - docs/adr/1221-gpu-ms-ssim-db-ceiling.md
 *   - docs/adr/1334-metal-ms-ssim-option-parity.md
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_metal.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#define FIXTURE_W 512u
#define FIXTURE_H 384u
#define FIXTURE_BPC 8u

/* MS-SSIM inherits the SSIM-family 1e-3 bound from ADR-0589. */
#define PARITY_TOL 1e-3

typedef struct MsSsimTestOptions {
    bool enable_db;
    bool clip_db;
    bool enable_chroma;
} MsSsimTestOptions;

static int fill_fixture(VmafPicture *pic, unsigned variant)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const int v = (int)((row + col) & 0xFFu);
            const int d = (variant != 0u) ? (((row * 5u + col) & 0x7) - 3) : 0;
            int clamped = v + d;
            if (clamped < 0)
                clamped = 0;
            if (clamped > 255)
                clamped = 255;
            y[row * pic->stride[0] + col] = (uint8_t)clamped;
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                const int v = (int)((row * 2u + col * 3u + p * 17u) & 0xFFu);
                const int d = (variant != 0u) ? (((row * 3u + col) & 0x7) - 3) : 0;
                int clamped = v + d;
                if (clamped < 0)
                    clamped = 0;
                if (clamped > 255)
                    clamped = 255;
                plane[row * pic->stride[p] + col] = (uint8_t)clamped;
            }
        }
    }
    return 0;
}

/* ADR-0141: keep branch budget within lint profile without suppression. */
static char *feed_fixture_pair_variant(VmafContext *vmaf, unsigned ref_variant,
                                       unsigned dist_variant)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_fixture(&ref, ref_variant);
    if (err)
        return "fill_fixture(ref) failed";
    err = fill_fixture(&dist, dist_variant);
    if (err) {
        vmaf_picture_unref(&ref);
        return "fill_fixture(dist) failed";
    }
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    if (err)
        return "vmaf_read_pictures failed";
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (err)
        return "vmaf_read_pictures(EOS) failed";
    return NULL;
}

static int make_options(const MsSsimTestOptions *options, VmafFeatureDictionary **opts)
{
    int err = 0;
    if (options && options->enable_db)
        err = vmaf_feature_dictionary_set(opts, "enable_db", "true");
    if (!err && options && options->clip_db)
        err = vmaf_feature_dictionary_set(opts, "clip_db", "true");
    if (!err && options && options->enable_chroma)
        err = vmaf_feature_dictionary_set(opts, "enable_chroma", "true");
    if (err)
        (void)vmaf_feature_dictionary_free(opts);
    return err;
}

static char *setup_cpu_float_ms_ssim(const MsSsimTestOptions *options, VmafContext **vmaf,
                                     VmafFeatureDictionary **opts)
{
    int err = make_options(options, opts);
    if (err)
        return "CPU: option construction failed";

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    err = vmaf_init(vmaf, cfg);
    if (err)
        return "CPU: vmaf_init failed";
    err = vmaf_use_feature(*vmaf, "float_ms_ssim", *opts);
    *opts = NULL; /* The valid call consumes options on every return path. */
    return err ? "CPU: vmaf_use_feature(float_ms_ssim) failed" : NULL;
}

static char *setup_metal_float_ms_ssim(VmafMetalState *mstate, const MsSsimTestOptions *options,
                                       VmafContext **vmaf, VmafFeatureDictionary **opts)
{
    int err = make_options(options, opts);
    if (err)
        return "Metal: option construction failed";

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    err = vmaf_init(vmaf, cfg);
    if (err)
        return "Metal: vmaf_init failed";
    err = vmaf_metal_import_state(*vmaf, mstate);
    if (err)
        return "Metal: vmaf_metal_import_state failed";
    err = vmaf_use_feature(*vmaf, "float_ms_ssim_metal", *opts);
    *opts = NULL; /* The valid call consumes options on every return path. */
    return err ? "Metal: vmaf_use_feature(float_ms_ssim_metal) failed" : NULL;
}

static char *collect_float_ms_ssim_scores(VmafContext *vmaf, bool metal, double *out_y,
                                          double *out_cb, double *out_cr)
{
    int err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim", out_y, 0u);
    if (err)
        return metal ? "Metal: float_ms_ssim read failed" : "CPU: float_ms_ssim read failed";
    if (out_cb) {
        err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim_cb", out_cb, 0u);
        if (err) {
            return metal ? "Metal: float_ms_ssim_cb read failed" :
                           "CPU: float_ms_ssim_cb read failed";
        }
    }
    if (out_cr) {
        err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim_cr", out_cr, 0u);
        if (err) {
            return metal ? "Metal: float_ms_ssim_cr read failed" :
                           "CPU: float_ms_ssim_cr read failed";
        }
    }
    return NULL;
}

static char *run_cpu_float_ms_ssim_opts(const MsSsimTestOptions *options, bool identical,
                                        double *out_y, double *out_cb, double *out_cr)
{
    VmafFeatureDictionary *opts = NULL;
    VmafContext *vmaf = NULL;
    char *result = setup_cpu_float_ms_ssim(options, &vmaf, &opts);
    if (!result) {
        result = feed_fixture_pair_variant(vmaf, 0u, identical ? 0u : 1u);
    }
    if (!result) {
        result = collect_float_ms_ssim_scores(vmaf, false, out_y, out_cb, out_cr);
    }

    if (opts)
        (void)vmaf_feature_dictionary_free(&opts);
    if (vmaf && vmaf_close(vmaf) != 0 && !result)
        result = "CPU: vmaf_close failed";
    return result;
}

static char *run_metal_float_ms_ssim_opts(const MsSsimTestOptions *options, bool identical,
                                          double *out_y, double *out_cb, double *out_cr)
{
    *out_y = NAN;
    if (out_cb)
        *out_cb = NAN;
    if (out_cr)
        *out_cr = NAN;

    VmafMetalConfiguration mcfg = {.device_index = -1, .flags = 0};
    VmafMetalState *mstate = NULL;
    int err = vmaf_metal_state_init(&mstate, mcfg);
    if (err != 0 || mstate == NULL) {
        (void)fprintf(stderr, "[skip: no Metal device] ");
        if (mstate)
            vmaf_metal_state_free(&mstate);
        return NULL;
    }

    VmafFeatureDictionary *opts = NULL;
    VmafContext *vmaf = NULL;
    char *result = setup_metal_float_ms_ssim(mstate, options, &vmaf, &opts);
    if (!result) {
        result = feed_fixture_pair_variant(vmaf, 0u, identical ? 0u : 1u);
    }
    if (!result) {
        result = collect_float_ms_ssim_scores(vmaf, true, out_y, out_cb, out_cr);
    }

    if (vmaf && vmaf_close(vmaf) != 0 && !result)
        result = "Metal: vmaf_close failed";
    if (opts)
        (void)vmaf_feature_dictionary_free(&opts);
    vmaf_metal_state_free(&mstate);
    return result;
}

static char *test_float_ms_ssim_cpu_metal_parity(void)
{
    double cpu_score = 0.0;
    double metal_score = NAN;

    char *msg = run_cpu_float_ms_ssim_opts(NULL, false, &cpu_score, NULL, NULL);
    if (msg)
        return msg;
    msg = run_metal_float_ms_ssim_opts(NULL, false, &metal_score, NULL, NULL);
    if (msg)
        return msg;
    if (isnan(metal_score))
        return NULL;

    const double delta = fabs(cpu_score - metal_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\nfloat_ms_ssim parity FAIL: cpu=%.8f metal=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, metal_score, delta, PARITY_TOL);
    }
    mu_assert("float_ms_ssim CPU vs. Metal exceeds 1e-3 tolerance (ADR-0589)", delta <= PARITY_TOL);
    return NULL;
}

static char *test_metal_float_ms_ssim_clip_db_ceiling(void)
{
    const MsSsimTestOptions options = {.enable_db = true, .clip_db = true};

    double cpu_score = 0.0;
    double metal_score = NAN;

    char *msg = run_cpu_float_ms_ssim_opts(&options, true, &cpu_score, NULL, NULL);
    if (msg)
        return msg;
    msg = run_metal_float_ms_ssim_opts(&options, true, &metal_score, NULL, NULL);
    if (msg)
        return msg;
    if (isnan(metal_score))
        return NULL;

    /* On identical frames with clip_db enabled, score must equal the geometry-derived max_db.
     * For 512x384 at 8-bit: peak=255, mse=0.5/(512*384), ceil(10*log10(peak*peak/mse)) = 105.0. */
    const double expected_ceiling = 105.0;
    mu_assert("CPU clip_db score must match ceiling", fabs(cpu_score - expected_ceiling) < 1e-4);
    mu_assert("Metal clip_db score must match ceiling",
              fabs(metal_score - expected_ceiling) < 1e-4);
    return NULL;
}

static char *test_metal_float_ms_ssim_parity_chroma(void)
{
    const MsSsimTestOptions options = {.enable_chroma = true};

    double cpu_y = 0.0;
    double cpu_cb = 0.0;
    double cpu_cr = 0.0;
    double metal_y = NAN;
    double metal_cb = NAN;
    double metal_cr = NAN;

    char *msg = run_cpu_float_ms_ssim_opts(&options, false, &cpu_y, &cpu_cb, &cpu_cr);
    if (msg)
        return msg;
    msg = run_metal_float_ms_ssim_opts(&options, false, &metal_y, &metal_cb, &metal_cr);
    if (msg)
        return msg;
    if (isnan(metal_y))
        return NULL;

    const double delta_y = fabs(cpu_y - metal_y);
    const double delta_cb = fabs(cpu_cb - metal_cb);
    const double delta_cr = fabs(cpu_cr - metal_cr);

    mu_assert("float_ms_ssim Y exceeds tolerance", delta_y <= PARITY_TOL);
    mu_assert("float_ms_ssim_cb exceeds tolerance", delta_cb <= PARITY_TOL);
    mu_assert("float_ms_ssim_cr exceeds tolerance", delta_cr <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_ms_ssim_cpu_metal_parity);
    mu_run_test(test_metal_float_ms_ssim_clip_db_ceiling);
    mu_run_test(test_metal_float_ms_ssim_parity_chroma);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
