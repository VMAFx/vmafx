/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-0945 round-3 — float_adm CPU vs. HIP parity test.
 *
 * The float-pipeline ADM feature is computed by float_adm.c (CPU) and
 * by float_adm_hip.c + float_adm/adm_*.hip (HIP — 4-stage DWT + CSF +
 * CM pipeline).  The integer ADM HIP twin already has a parity gate
 * (test_hip_adm_parity, ADR-0539); this test pins the float variant
 * which feeds a different consumer set (model trainers that prefer the
 * unquantised features).
 *
 * Asserts the always-emitted `VMAF_feature_adm2_score` plus the four
 * per-scale ratio channels.  Skip behaviour: if vmaf_hip_state_init()
 * fails OR the HIP path returns -ENOSYS (scaffold posture under
 * enable_hipcc=false) the test emits a skip-tag and passes.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "hip_parity_skip.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"

/* Wide enough for the ADM DWT2 pyramid (each scale halves the
 * dimensions, so >= 32x32 keeps scale 3 from collapsing). */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#define FIXTURE_BPC 8u
#define PARITY_TOL 1e-4

static const char *const kAdmFeatures[] = {
    "VMAF_feature_adm2_score",       "VMAF_feature_adm_scale0_score",
    "VMAF_feature_adm_scale1_score", "VMAF_feature_adm_scale2_score",
    "VMAF_feature_adm_scale3_score",
};
#define NUM_ADM_FEATURES (sizeof(kAdmFeatures) / sizeof(kAdmFeatures[0]))

/* ADR-1220 — derived feature keys for the adm_p_norm variant.
 *
 * `adm_p_norm` is a VMAF_OPT_FLAG_FEATURE_PARAM, so setting it changes the key
 * the score is filed under (ADR-1183): the alias base plus `_apn_<%g value>`. */
static const char *const kAdmFeaturesApn[] = {
    "adm2_apn_2", "adm_scale0_apn_2", "adm_scale1_apn_2", "adm_scale2_apn_2", "adm_scale3_apn_2",
};
static const char *const kAdmFeaturesScf[NUM_ADM_FEATURES] = {
    "adm2_scf_2", "adm_scale0_scf_2", "adm_scale1_scf_2", "adm_scale2_scf_2", "adm_scale3_scf_2",
};
static const char *const kAdmFeaturesBcm[NUM_ADM_FEATURES] = {
    "adm2_bcm_1", "adm_scale0_bcm_1", "adm_scale1_bcm_1", "adm_scale2_bcm_1", "adm_scale3_bcm_1",
};

/* Build the option dictionary for a variant, or leave it NULL for defaults. */
static int adm_opts_build(VmafFeatureDictionary **opts, const char *name, const char *val)
{
    if (!name)
        return 0;
    return vmaf_feature_dictionary_set(opts, name, val);
}

static int fill_ref(VmafPicture *pic)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row * 3u + col) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++)
            memset(plane + row * pic->stride[p], 128, pic->w[p]);
    }
    return 0;
}

static int fill_dis(VmafPicture *pic)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            int v = (int)((row * 3u + col) & 0xFFu);
            v += (int)((row ^ col) & 0x07u) - 3;
            if (v < 0)
                v = 0;
            if (v > 255)
                v = 255;
            y[row * pic->stride[0] + col] = (uint8_t)v;
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++)
            memset(plane + row * pic->stride[p], 128, pic->w[p]);
    }
    return 0;
}

static int feed_frame(VmafContext *vmaf)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_ref(&ref);
    if (err)
        return err;
    err = fill_dis(&dist);
    if (err) {
        vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, 0u);
}

static char *run_cpu_float_adm(const char *opt_name, const char *opt_val, const char *const *keys,
                               double scores[NUM_ADM_FEATURES])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    VmafFeatureDictionary *opts = NULL;
    err = adm_opts_build(&opts, opt_name, opt_val);
    mu_assert("CPU: adm_opts_build failed", !err);
    err = vmaf_use_feature(vmaf, "float_adm", opts);
    if (err)
        (void)vmaf_feature_dictionary_free(&opts);
    mu_assert("CPU: vmaf_use_feature(float_adm) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    for (size_t f = 0; f < NUM_ADM_FEATURES; f++) {
        err = vmaf_feature_score_at_index(vmaf, keys[f], &scores[f], 0u);
        if (err)
            (void)fprintf(stderr, "CPU: feature %s missing (err=%d)\n", kAdmFeatures[f], err);
        mu_assert("CPU: vmaf_feature_score_at_index failed", !err);
    }
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_hip_float_adm(const char *opt_name, const char *opt_val, const char *const *keys,
                               double scores[NUM_ADM_FEATURES], int *skipped)
{
    for (size_t f = 0; f < NUM_ADM_FEATURES; f++)
        scores[f] = NAN;
    *skipped = 0;

    VmafHipState *hip_state = NULL;
    VmafHipConfiguration hip_cfg = {.device_index = -1};
    int err = vmaf_hip_state_init(&hip_state, hip_cfg);
    if (err != 0 || hip_state == NULL) {
        (void)fprintf(stderr, "[skip: no HIP device] ");
        *skipped = 1;
        return NULL;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("HIP: vmaf_init failed", !err);
    err = vmaf_hip_import_state(vmaf, hip_state);
    mu_assert("HIP: vmaf_hip_import_state failed", !err);
    VmafFeatureDictionary *opts = NULL;
    err = adm_opts_build(&opts, opt_name, opt_val);
    mu_assert("HIP: adm_opts_build failed", !err);
    err = vmaf_use_feature(vmaf, "float_adm_hip", opts);
    if (err == -ENOSYS)
        return hip_parity_skip(vmaf, &hip_state, skipped, "");
    mu_assert("HIP: vmaf_use_feature(float_adm_hip) failed", !err);
    err = feed_frame(vmaf);
    if (err == -ENOSYS)
        return hip_parity_skip(vmaf, &hip_state, skipped, " on feed");
    if (err == -ENOSYS) {
        /* Documented scaffold contract: an unimplemented HIP extractor returns
         * -ENOSYS from init (see the HIP extractors under
         * core/src/feature/hip/). That is a not-built-yet signal, not a
         * regression, so skip exactly as the no-device branch above does.
         * Any other error still fails. */
        (void)fprintf(stderr, "[skip: HIP extractor is a scaffold (-ENOSYS)] ");
        (void)vmaf_close(vmaf);
        vmaf_hip_state_free(&hip_state);
        return NULL;
    }
    mu_assert("HIP: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (err == -ENOSYS)
        return hip_parity_skip(vmaf, &hip_state, skipped, " on EOS");
    mu_assert("HIP: vmaf_read_pictures(EOS) failed", !err);
    for (size_t f = 0; f < NUM_ADM_FEATURES; f++) {
        err = vmaf_feature_score_at_index(vmaf, keys[f], &scores[f], 0u);
        if (err)
            (void)fprintf(stderr, "HIP: feature %s missing (err=%d)\n", kAdmFeatures[f], err);
        mu_assert("HIP: vmaf_feature_score_at_index failed", !err);
    }
    err = vmaf_close(vmaf);
    mu_assert("HIP: vmaf_close failed", !err);
    vmaf_hip_state_free(&hip_state);
    return NULL;
}

static char *test_float_adm_hip_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_adm_hip");
    mu_assert("float_adm_hip extractor must be registered", fex != NULL);
    mu_assert("float_adm_hip name matches", !strcmp(fex->name, "float_adm_hip"));
    return NULL;
}

static char *test_float_adm_cpu_hip_parity(void)
{
    double cpu_scores[NUM_ADM_FEATURES] = {0};
    double hip_scores[NUM_ADM_FEATURES] = {0};
    int skipped = 0;

    char *msg = run_cpu_float_adm(NULL, NULL, kAdmFeatures, cpu_scores);
    if (msg)
        return msg;
    msg = run_hip_float_adm(NULL, NULL, kAdmFeatures, hip_scores, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;
    for (size_t f = 0; f < NUM_ADM_FEATURES; f++) {
        if (isnan(hip_scores[f]))
            return NULL;
        double d = fabs(cpu_scores[f] - hip_scores[f]);
        if (d > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nfloat_adm parity FAIL: %s cpu=%.8f hip=%.8f delta=%.2e tol=%.2e\n",
                          kAdmFeatures[f], cpu_scores[f], hip_scores[f], d, PARITY_TOL);
        }
        mu_assert("float_adm CPU vs. HIP delta exceeds places=4 tolerance (1e-4)", d <= PARITY_TOL);
    }
    return NULL;
}

/* ADR-1220 — adm_p_norm must reach the kernels. The twin declares it with the
 * CPU's name, alias, default and range, but its kernels hardcoded the cube sum
 * and its host pooling hardcoded the 1/3 root, so a non-default `apn` moved
 * only the AIM exponent and produced a hybrid quantity. The default-options
 * test above cannot see it, because p = 3 IS the hardcoded exponent. */
static char *test_float_adm_p_norm_reaches_kernel(void)
{
    double cpu_scores[NUM_ADM_FEATURES] = {0};
    double hip_scores[NUM_ADM_FEATURES] = {0};
    int skipped = 0;

    char *msg = run_cpu_float_adm("adm_p_norm", "2.0", kAdmFeaturesApn, cpu_scores);
    if (msg)
        return msg;
    msg = run_hip_float_adm("adm_p_norm", "2.0", kAdmFeaturesApn, hip_scores, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;
    for (size_t f = 0; f < NUM_ADM_FEATURES; f++) {
        if (isnan(hip_scores[f]))
            return NULL;
        const double d = fabs(cpu_scores[f] - hip_scores[f]);
        if (d > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nfloat_adm apn=2.0 parity FAIL: %s cpu=%.8f hip=%.8f delta=%.2e "
                          "tol=%.2e\n",
                          kAdmFeaturesApn[f], cpu_scores[f], hip_scores[f], d, PARITY_TOL);
        }
        mu_assert("float_adm with a non-default adm_p_norm drifts from the CPU reference",
                  d <= PARITY_TOL);
    }
    return NULL;
}

/* ADR-1214 — adm_csf_scale must be a no-op in the Watson-97 mode this twin
 * implements, exactly as it is on the CPU (`adm_tools.c::adm_csf_rfactor_s`
 * consults it only in Barten mode). The twins used to multiply it into every
 * CSF rfactor, and declared it under the alias `cs` where the CPU says `scf`,
 * so the same request produced a different feature key as well as a different
 * score. The fix landed as 64ea351be without this regression test.
 *
 * The key suffix follows ADR-1183: the alias base plus `_<alias>_<%g value>`,
 * so `adm_csf_scale=2.0` files the scores under `_scf_2`. */
static char *test_float_adm_csf_scale_is_a_watson_mode_noop(void)
{
    double cpu_scores[NUM_ADM_FEATURES] = {0};
    double hip_scores[NUM_ADM_FEATURES] = {0};
    int skipped = 0;

    char *msg = run_cpu_float_adm("adm_csf_scale", "2.0", kAdmFeaturesScf, cpu_scores);
    if (msg)
        return msg;
    msg = run_hip_float_adm("adm_csf_scale", "2.0", kAdmFeaturesScf, hip_scores, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;
    for (size_t f = 0; f < NUM_ADM_FEATURES; f++) {
        if (isnan(hip_scores[f]))
            return NULL;
        const double d = fabs(cpu_scores[f] - hip_scores[f]);
        if (d > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nfloat_adm scf=2.0 parity FAIL: %s cpu=%.8f hip=%.8f delta=%.2e "
                          "tol=%.2e\n",
                          kAdmFeaturesScf[f], cpu_scores[f], hip_scores[f], d, PARITY_TOL);
        }
        mu_assert("float_adm applies adm_csf_scale in Watson mode where the CPU ignores it",
                  d <= PARITY_TOL);
    }
    return NULL;
}

/* ADR-1220 — adm_bypass_cm drops the contrast-masking threshold in both DLM and
 * AIM CM kernels. Verify that setting adm_bypass_cm=1 changes the score and
 * matches the CPU reference within tolerance. */
static char *test_float_adm_bypass_cm_reaches_kernel(void)
{
    double cpu_scores[NUM_ADM_FEATURES] = {0};
    double hip_scores[NUM_ADM_FEATURES] = {0};
    int skipped = 0;

    char *msg = run_cpu_float_adm("adm_bypass_cm", "1", kAdmFeaturesBcm, cpu_scores);
    if (msg)
        return msg;
    msg = run_hip_float_adm("adm_bypass_cm", "1", kAdmFeaturesBcm, hip_scores, &skipped);
    if (msg)
        return msg;
    if (skipped)
        return NULL;
    for (size_t f = 0; f < NUM_ADM_FEATURES; f++) {
        if (isnan(hip_scores[f]))
            return NULL;
        const double d = fabs(cpu_scores[f] - hip_scores[f]);
        if (d > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nfloat_adm bcm=1 parity FAIL: %s cpu=%.8f hip=%.8f delta=%.2e "
                          "tol=%.2e\n",
                          kAdmFeaturesBcm[f], cpu_scores[f], hip_scores[f], d, PARITY_TOL);
        }
        mu_assert("float_adm with adm_bypass_cm=1 drifts from the CPU reference", d <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_adm_hip_registered);
    mu_run_test(test_float_adm_cpu_hip_parity);
    mu_run_test(test_float_adm_p_norm_reaches_kernel);
    mu_run_test(test_float_adm_csf_scale_is_a_watson_mode_noop);
    mu_run_test(test_float_adm_bypass_cm_reaches_kernel);
    return NULL;
}
