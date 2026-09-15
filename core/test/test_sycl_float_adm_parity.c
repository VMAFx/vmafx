/**
 *
 *  Copyright 2026 Lusoris
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

/*
 * SYCL kernel coverage round 3 — float ADM CPU vs. SYCL parity test
 * (ADR-0946).
 *
 * The float ADM extractor is implemented by float_adm.c (CPU scalar /
 * SIMD via adm_tools.c) and by float_adm_sycl.cpp::vmaf_fex_float_adm_sycl
 * (SYCL DWT2 + CSF + contrast-masking pipeline). Round 2 covered the
 * integer ADM kernel (test_sycl_adm_parity.c); the float variant has
 * its own DWT topology, accumulator precision, and CSF lookup and
 * needs its own parity gate.
 *
 * The kernel under test fans a 5x5 separable DWT2 over the luma plane,
 * computes per-subband contrast-masking scores against a CSF lookup,
 * and reduces to the VMAF_feature_adm2_score headline column — any
 * USM stride, sub-group mask, or 32-bit accumulator drift would
 * silently corrupt every float-VMAF model's primary feature on
 * Intel-Arc CHUG re-extracts.
 *
 * Skip behaviour: if vmaf_sycl_state_init() fails (no oneAPI runtime
 * or no device visible) the test emits "[skip: no SYCL device]" and
 * passes, mirroring test_sycl_motion3_parity.c.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

/* Fixture must be ≥ 32x32 for the 4-scale DWT2 + CSF footprint;
 * 256x144 matches the round-2 ADM parity test sizing. */
#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u
#define PARITY_TOL 1e-4

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            /* XOR pattern + salt — gives non-zero variance across
             * each DWT2 subband. */
            y[row * pic->stride[0] + col] = (uint8_t)(((row ^ col) + salt * 17u) & 0xFFu);
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

static int feed_frame(VmafContext *vmaf)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_pic(&ref, 0u);
    if (err)
        return err;
    err = fill_pic(&dist, 1u);
    if (err) {
        vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, 0u);
}

/* ADR-1220 — `adm_p_norm` is a VMAF_OPT_FLAG_FEATURE_PARAM, so setting it
 * changes the key the score is filed under (ADR-1183): the alias base plus
 * `_apn_<%g value>`. */
#define APN_VAL "2.0"

/* Compare all five ADM features, not just the aggregate: the per-scale
 * sub-scores are where a kernel-vs-CPU divergence shows first, and on this
 * fixture the aggregate alone is not sensitive enough to see the p-norm
 * defect at all. */
#define NUM_ADM_FEATURES 5u
static const char *const kAdmFeatures[NUM_ADM_FEATURES] = {
    "VMAF_feature_adm2_score",       "VMAF_feature_adm_scale0_score",
    "VMAF_feature_adm_scale1_score", "VMAF_feature_adm_scale2_score",
    "VMAF_feature_adm_scale3_score",
};
static const char *const kAdmFeaturesApn[NUM_ADM_FEATURES] = {
    "adm2_apn_2", "adm_scale0_apn_2", "adm_scale1_apn_2", "adm_scale2_apn_2", "adm_scale3_apn_2",
};

/* Build the option dictionary for a variant, or leave it NULL for defaults. */
static int adm_opts_build(VmafFeatureDictionary **opts, const char *name, const char *val)
{
    if (!name)
        return 0;
    return vmaf_feature_dictionary_set(opts, name, val);
}

static char *run_cpu(const char *opt_name, const char *opt_val, const char *const *keys,
                     double *scores)
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
    for (unsigned m = 0; m < NUM_ADM_FEATURES; m++) {
        err = vmaf_feature_score_at_index(vmaf, keys[m], &scores[m], 0u);
        mu_assert("CPU: float_adm score missing", !err);
    }
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_sycl(const char *opt_name, const char *opt_val, const char *const *keys,
                      double *scores)
{
    for (unsigned m = 0; m < NUM_ADM_FEATURES; m++)
        scores[m] = NAN;
    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    int err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err != 0 || sycl_state == NULL) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return NULL;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("SYCL: vmaf_init failed", !err);
    err = vmaf_sycl_import_state(vmaf, sycl_state);
    mu_assert("SYCL: vmaf_sycl_import_state failed", !err);
    VmafFeatureDictionary *opts = NULL;
    err = adm_opts_build(&opts, opt_name, opt_val);
    mu_assert("SYCL: adm_opts_build failed", !err);
    err = vmaf_use_feature(vmaf, "float_adm_sycl", opts);
    if (err)
        (void)vmaf_feature_dictionary_free(&opts);
    mu_assert("SYCL: vmaf_use_feature(float_adm_sycl) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("SYCL: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("SYCL: vmaf_read_pictures(EOS) failed", !err);
    for (unsigned m = 0; m < NUM_ADM_FEATURES; m++) {
        err = vmaf_feature_score_at_index(vmaf, keys[m], &scores[m], 0u);
        mu_assert("SYCL: float_adm score missing", !err);
    }
    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);
    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

static char *test_float_adm_sycl_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_adm_sycl");
    mu_assert("float_adm_sycl extractor must be registered", fex != NULL);
    mu_assert("float_adm_sycl name matches", !strcmp(fex->name, "float_adm_sycl"));
    return NULL;
}

static char *test_float_adm_cpu_sycl_parity(void)
{
    double cpu_scores[NUM_ADM_FEATURES] = {0};
    double sycl_scores[NUM_ADM_FEATURES] = {0};
    char *msg = run_cpu(NULL, NULL, kAdmFeatures, cpu_scores);
    if (msg)
        return msg;
    msg = run_sycl(NULL, NULL, kAdmFeatures, sycl_scores);
    if (msg)
        return msg;
    if (isnan(sycl_scores[0]))
        return NULL;
    for (unsigned m = 0; m < NUM_ADM_FEATURES; m++) {
        const double delta = fabs(cpu_scores[m] - sycl_scores[m]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nfloat_adm parity FAIL: %s cpu=%.8f sycl=%.8f delta=%.2e tol=%.2e\n",
                          kAdmFeatures[m], cpu_scores[m], sycl_scores[m], delta, PARITY_TOL);
        }
        mu_assert("float_adm CPU vs. SYCL delta exceeds places=4 tolerance (1e-4)",
                  delta <= PARITY_TOL);
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
    double sycl_scores[NUM_ADM_FEATURES] = {0};
    char *msg = run_cpu("adm_p_norm", APN_VAL, kAdmFeaturesApn, cpu_scores);
    if (msg)
        return msg;
    msg = run_sycl("adm_p_norm", APN_VAL, kAdmFeaturesApn, sycl_scores);
    if (msg)
        return msg;
    if (isnan(sycl_scores[0]))
        return NULL;
    for (unsigned m = 0; m < NUM_ADM_FEATURES; m++) {
        const double delta = fabs(cpu_scores[m] - sycl_scores[m]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nfloat_adm apn=%s parity FAIL: %s cpu=%.8f sycl=%.8f delta=%.2e "
                          "tol=%.2e\n",
                          APN_VAL, kAdmFeaturesApn[m], cpu_scores[m], sycl_scores[m], delta,
                          PARITY_TOL);
        }
        mu_assert("float_adm with a non-default adm_p_norm drifts from the CPU reference",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_adm_sycl_registered);
    mu_run_test(test_float_adm_cpu_sycl_parity);
    mu_run_test(test_float_adm_p_norm_reaches_kernel);
    return NULL;
}
