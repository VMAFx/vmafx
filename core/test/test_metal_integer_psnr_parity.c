/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Metal kernel coverage round 2 — integer_psnr CPU vs. Metal parity.
 *
 * Beyond the registration audit in PR #351, this test exercises the real
 * integer_psnr Metal kernel on a synthetic 8-bpc YUV420P fixture and
 * compares its psnr_y / psnr_cb / psnr_cr scores against the CPU `psnr`
 * extractor. PSNR is a single-frame metric so a one-frame fixture is
 * enough.
 *
 * Tolerance: places=4 (1e-4 dB) per ADR-0214 cross-backend gate. The Metal
 * kernel computes SSE in 64-bit lane-summed integer arithmetic and converts
 * to dB via `10 * log10(peak^2 / mse)` (see `integer_psnr_metal.mm` extract
 * block); the CPU twin uses the same formula, so the residual drift is
 * limited to the order in which workgroup partials accumulate. 1e-4 dB
 * is the same gate the SYCL motion3 parity twin uses.
 *
 * Skip behaviour: when `vmaf_metal_state_init` returns -ENODEV (Linux,
 * Windows, Intel Mac), the test emits "[skip: no Metal device]" and passes
 * cleanly.
 *
 * Cross-references:
 *   - core/src/feature/metal/integer_psnr_metal.mm
 *   - core/src/feature/integer_psnr.c
 *   - docs/adr/0214-gpu-parity-ci-gate.md
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "dict.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_metal.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u

/* PSNR is reported in dB; places=4 (1e-4 dB) per ADR-0214. */
#define PARITY_TOL 1e-4

static int fill_fixture_ref(VmafPicture *pic)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    /* Deterministic luma ramp, mid-grey chroma. */
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            memset(plane + row * pic->stride[p], 128, pic->w[p]);
        }
    }
    return 0;
}

static int fill_fixture_dist(VmafPicture *pic)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    /* Distorted = ref + small per-pixel perturbation; keeps PSNR finite
     * (well below psnr_max) so the log10 path is exercised. */
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const int v = (int)((row + col) & 0xFFu);
            const int d = ((row * 3u + col) & 0x7) - 4; /* signed delta in [-4..3] */
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
                /* Chroma drift so psnr_cb/psnr_cr are also finite. */
                plane[row * pic->stride[p] + col] = (uint8_t)(128 + (int)((row + col) & 0x3) - 2);
            }
        }
    }
    return 0;
}

typedef struct {
    double psnr_y;
    double psnr_cb;
    double psnr_cr;
} PsnrTriple;

/* Both sides feed the same fixture pair, so the sequence lives here once.
 * Extracting it also keeps each run_* function inside the branch budget the
 * lint profile sets, which is the refactor ADR-0141 asks for rather than a
 * suppression. */
/* Opening a Metal-backed context is the same two calls every time; folding
 * them into one keeps run_metal inside the branch budget (ADR-0141). */
static char *open_metal_context(VmafContext **vmaf, VmafMetalState *mstate)
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    int err = vmaf_init(vmaf, cfg);
    if (err)
        return "Metal: vmaf_init failed";
    err = vmaf_metal_import_state(*vmaf, mstate);
    if (err)
        return "Metal: vmaf_metal_import_state failed";
    return NULL;
}

static char *feed_fixture_pair(VmafContext *vmaf)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_fixture_ref(&ref);
    if (err)
        return "fill_fixture_ref failed";
    err = fill_fixture_dist(&dist);
    if (err)
        return "fill_fixture_dist failed";
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    if (err)
        return "vmaf_read_pictures failed";
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (err)
        return "vmaf_read_pictures(EOS) failed";
    return NULL;
}

static char *run_cpu_psnr(PsnrTriple *out)
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "psnr", NULL);
    mu_assert("CPU: vmaf_use_feature(psnr) failed", !err);

    char *feed_err = feed_fixture_pair(vmaf);
    if (feed_err)
        return feed_err;

    err = vmaf_feature_score_at_index(vmaf, "psnr_y", &out->psnr_y, 0u);
    mu_assert("CPU: psnr_y read failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_cb", &out->psnr_cb, 0u);
    mu_assert("CPU: psnr_cb read failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_cr", &out->psnr_cr, 0u);
    mu_assert("CPU: psnr_cr read failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_metal_psnr(PsnrTriple *out)
{
    out->psnr_y = out->psnr_cb = out->psnr_cr = NAN;
    int err = 0;

    VmafMetalConfiguration mcfg = {.device_index = -1, .flags = 0};
    VmafMetalState *mstate = NULL;
    err = vmaf_metal_state_init(&mstate, mcfg);
    if (err != 0 || mstate == NULL) {
        (void)fprintf(stderr, "[skip: no Metal device] ");
        return NULL;
    }

    VmafContext *vmaf = NULL;
    char *open_err = open_metal_context(&vmaf, mstate);
    if (open_err)
        return open_err;
    err = vmaf_use_feature(vmaf, "integer_psnr_metal", NULL);
    mu_assert("Metal: vmaf_use_feature(integer_psnr_metal) failed", !err);

    char *feed_err = feed_fixture_pair(vmaf);
    if (feed_err)
        return feed_err;

    err = vmaf_feature_score_at_index(vmaf, "psnr_y", &out->psnr_y, 0u);
    mu_assert("Metal: psnr_y read failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_cb", &out->psnr_cb, 0u);
    mu_assert("Metal: psnr_cb read failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_cr", &out->psnr_cr, 0u);
    mu_assert("Metal: psnr_cr read failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("Metal: vmaf_close failed", !err);
    vmaf_metal_state_free(&mstate);
    return NULL;
}

static char *test_integer_psnr_cpu_metal_parity(void)
{
    PsnrTriple cpu = {0};
    PsnrTriple metal = {NAN, NAN, NAN};
    char *msg = run_cpu_psnr(&cpu);
    if (msg)
        return msg;
    msg = run_metal_psnr(&metal);
    if (msg)
        return msg;
    if (isnan(metal.psnr_y))
        return NULL;

    const double d_y = fabs(cpu.psnr_y - metal.psnr_y);
    const double d_cb = fabs(cpu.psnr_cb - metal.psnr_cb);
    const double d_cr = fabs(cpu.psnr_cr - metal.psnr_cr);
    if (d_y > PARITY_TOL || d_cb > PARITY_TOL || d_cr > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\npsnr parity FAIL: y(cpu=%.6f metal=%.6f d=%.2e) cb(d=%.2e) cr(d=%.2e) "
                      "tol=%.2e\n",
                      cpu.psnr_y, metal.psnr_y, d_y, d_cb, d_cr, PARITY_TOL);
    }
    mu_assert("psnr_y CPU vs. Metal exceeds 1e-4 dB", d_y <= PARITY_TOL);
    mu_assert("psnr_cb CPU vs. Metal exceeds 1e-4 dB", d_cb <= PARITY_TOL);
    mu_assert("psnr_cr CPU vs. Metal exceeds 1e-4 dB", d_cr <= PARITY_TOL);
    return NULL;
}

static char *test_integer_psnr_metal_enable_chroma_false(void)
{
    VmafMetalConfiguration mcfg = {.device_index = -1, .flags = 0};
    VmafMetalState *mstate = NULL;
    int err = vmaf_metal_state_init(&mstate, mcfg);
    if (err != 0 || mstate == NULL) {
        (void)fprintf(stderr, "[skip: no Metal device] ");
        return NULL;
    }

    VmafDictionary *opts = NULL;
    err = vmaf_dictionary_set(&opts, "enable_chroma", "false", 0);
    mu_assert("vmaf_dictionary_set(enable_chroma=false) failed", !err);

    VmafContext *vmaf = NULL;
    char *open_err = open_metal_context(&vmaf, mstate);
    if (open_err) {
        (void)vmaf_dictionary_free(&opts);
        vmaf_metal_state_free(&mstate);
        return open_err;
    }

    err = vmaf_use_feature(vmaf, "integer_psnr_metal", opts);
    (void)vmaf_dictionary_free(&opts);
    mu_assert("Metal: vmaf_use_feature(integer_psnr_metal, enable_chroma=false) failed", !err);

    char *feed_err = feed_fixture_pair(vmaf);
    if (feed_err) {
        (void)vmaf_close(vmaf);
        vmaf_metal_state_free(&mstate);
        return feed_err;
    }

    double psnr_y = NAN;
    err = vmaf_feature_score_at_index(vmaf, "psnr_y", &psnr_y, 0u);
    mu_assert("Metal: psnr_y read failed", !err);
    mu_assert("Metal: psnr_y must be finite", !isnan(psnr_y));

    double dummy = NAN;
    err = vmaf_feature_score_at_index(vmaf, "psnr_cb", &dummy, 0u);
    mu_assert("Metal: psnr_cb must not be emitted when enable_chroma=false", err != 0);

    err = vmaf_feature_score_at_index(vmaf, "psnr_cr", &dummy, 0u);
    mu_assert("Metal: psnr_cr must not be emitted when enable_chroma=false", err != 0);

    err = vmaf_close(vmaf);
    mu_assert("Metal: vmaf_close failed", !err);
    vmaf_metal_state_free(&mstate);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_integer_psnr_cpu_metal_parity);
    mu_run_test(test_integer_psnr_metal_enable_chroma_false);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
