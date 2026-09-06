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
 * SYCL kernel coverage round 2 — ADM CPU vs. SYCL parity test (ADR-0884).
 *
 * ADM (Additive Detail Measure, scale 0..3) is computed by integer_adm.c
 * (CPU scalar / SIMD) and by integer_adm_sycl.cpp (SYCL kernel, 1738
 * lines — the largest and most complex SYCL kernel in the fork). Before
 * this test there was NO cross-backend parity gate for adm_sycl. ADM
 * is the dominant feature in every shipping VMAF model
 * (libvmaf-2.x default + 4k + phone-screen). A regression in the SYCL
 * DLM (Detail-Loss Metric) sub-band convolution or the CSF (Contrast
 * Sensitivity Function) weighting would silently shift the headline
 * VMAF score on every Intel-Arc CHUG re-extract.
 *
 * The test asserts VMAF_integer_feature_adm2_score (the headline
 * combined-scale ADM2 score) matches between CPU and SYCL within
 * ADR-0214 places=4 (1e-4) tolerance.
 *
 * Skip behaviour: if vmaf_sycl_state_init() fails (no oneAPI runtime
 * or no device visible) the test emits "[skip: no SYCL device]" and
 * passes, mirroring test_sycl_motion3_parity.c.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

/* Fixture geometry — large enough to clear ADM's 5-tap filter and
 * 4-scale dyadic pyramid (min 32x32 after scale-3 decimation), small
 * enough for fast CI. */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
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
            /* Diagonal gradient + salt offset — produces non-trivial
             * detail at every ADM scale so DLM/CSF paths are exercised. */
            y[row * pic->stride[0] + col] = (uint8_t)(((row * 3u + col * 5u + salt * 17u)) & 0xFFu);
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

static char *run_cpu_adm(double *adm2)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "adm", NULL);
    mu_assert("CPU: vmaf_use_feature(adm) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_adm2_score", adm2, 0u);
    mu_assert("CPU: VMAF_integer_feature_adm2_score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_sycl_adm(double *adm2)
{
    *adm2 = NAN;
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
    err = vmaf_use_feature(vmaf, "adm_sycl", NULL);
    mu_assert("SYCL: vmaf_use_feature(adm_sycl) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("SYCL: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("SYCL: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "VMAF_integer_feature_adm2_score", adm2, 0u);
    mu_assert("SYCL: VMAF_integer_feature_adm2_score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);
    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

/* The default model `vmaf_v1.0.16_3d0h` asks integer ADM for
 * VMAF_integer_feature_adm3_score with adm_csf_mode=2, adm_dlm_weight=0.7,
 * adm_enhn_gain_limit=1.0, adm_min_val=0.5 and adm_noise_weight=0.02. Every
 * one of them is a non-default VMAF_OPT_FLAG_FEATURE_PARAM, so the key both
 * twins must emit carries all five suffixes (feature_name.cpp builds the key
 * from the extractor's own option table).
 *
 * adm_p_norm is deliberately left at its default here, unlike the CUDA twin's
 * copy of this test: at adm_p_norm=2 the scores leave [0, 1] (~3.25 on this
 * fixture) and the Arc A380's fp32 device accumulation lands 2.28e-03 away
 * from the CPU — the same fixture-specific amplification that already makes
 * test_adm_cpu_sycl_parity red at 1.10e-04
 * (T-SYCL-ARC-ADM2-PARITY-1.1E-4-2026-09-05 in docs/state.md). p_norm IS
 * honoured by this twin; it is covered on real 576x324 content in
 * python/test/gpu_default_model_test.py, where the delta is <= 5e-06. */
static VmafFeatureDictionary *model_opts(void)
{
    VmafFeatureDictionary *d = NULL;
    if (vmaf_feature_dictionary_set(&d, "adm_csf_mode", "2"))
        return NULL;
    if (vmaf_feature_dictionary_set(&d, "adm_dlm_weight", "0.7"))
        return NULL;
    if (vmaf_feature_dictionary_set(&d, "adm_enhn_gain_limit", "1.0"))
        return NULL;
    if (vmaf_feature_dictionary_set(&d, "adm_min_val", "0.5"))
        return NULL;
    if (vmaf_feature_dictionary_set(&d, "adm_noise_weight", "0.02"))
        return NULL;
    return d;
}

#define MODEL_SUFFIX "_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02"
/* adm3 / aim are deliberately absent: the SYCL twin has no AIM device pass
 * (see test_adm_sycl_does_not_claim_aim below), so those two features route to
 * the CPU twin through the ADR-0530 fallback instead. */
static const char *const MODEL_KEYS[] = {
    "integer_adm2" MODEL_SUFFIX,       "integer_adm_scale0" MODEL_SUFFIX,
    "integer_adm_scale1" MODEL_SUFFIX, "integer_adm_scale2" MODEL_SUFFIX,
    "integer_adm_scale3" MODEL_SUFFIX,
};
#define NUM_MODEL_KEYS (sizeof(MODEL_KEYS) / sizeof(MODEL_KEYS[0]))

static char *run_adm_with_model_opts(bool use_sycl, double out[NUM_MODEL_KEYS])
{
    for (unsigned k = 0; k < NUM_MODEL_KEYS; k++)
        out[k] = NAN;

    VmafSyclState *sycl_state = NULL;
    if (use_sycl) {
        VmafSyclConfiguration sycl_cfg = {.device_index = -1};
        const int sy_err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
        if (sy_err != 0 || sycl_state == NULL) {
            (void)fprintf(stderr, "[skip: no SYCL device] ");
            return NULL;
        }
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("model-opts: vmaf_init failed", !err);
    if (use_sycl) {
        err = vmaf_sycl_import_state(vmaf, sycl_state);
        mu_assert("model-opts: vmaf_sycl_import_state failed", !err);
    }

    VmafFeatureDictionary *opts = model_opts();
    mu_assert("model-opts: dictionary build failed", opts != NULL);
    err = vmaf_use_feature(vmaf, use_sycl ? "adm_sycl" : "adm", opts);
    mu_assert("model-opts: vmaf_use_feature failed", !err);

    err = feed_frame(vmaf);
    mu_assert("model-opts: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("model-opts: vmaf_read_pictures(EOS) failed", !err);

    for (unsigned k = 0; k < NUM_MODEL_KEYS; k++) {
        err = vmaf_feature_score_at_index(vmaf, MODEL_KEYS[k], &out[k], 0u);
        if (err) {
            (void)fprintf(stderr, "\nmissing feature-name key: %s (%s twin)\n", MODEL_KEYS[k],
                          use_sycl ? "SYCL" : "CPU");
        }
        mu_assert("model-opts: feature-name key not emitted", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("model-opts: vmaf_close failed", !err);
    if (use_sycl)
        vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

/* Every option the CPU table declares must also exist, with the same alias,
 * type and feature-param flag, in the SYCL table — otherwise the emitted
 * feature-name key diverges. */
static char *test_adm_sycl_option_table_mirrors_cpu(void)
{
    VmafFeatureExtractor *cpu = vmaf_get_feature_extractor_by_name("adm");
    VmafFeatureExtractor *gpu = vmaf_get_feature_extractor_by_name("adm_sycl");
    mu_assert("adm extractor must be registered", cpu != NULL);
    mu_assert("adm_sycl extractor must be registered", gpu != NULL);
    mu_assert("adm must declare options", cpu->options != NULL);
    mu_assert("adm_sycl must declare options", gpu->options != NULL);

    for (unsigned i = 0; cpu->options[i].name; i++) {
        const VmafOption *a = &cpu->options[i];
        /* adm_skip_aim is not a feature param and only drives the AIM pass,
         * which this twin does not have — it is correctly absent rather than
         * declared-and-ignored. */
        if (!strcmp(a->name, "adm_skip_aim"))
            continue;
        const VmafOption *b = NULL;
        for (unsigned j = 0; gpu->options[j].name; j++) {
            if (!strcmp(gpu->options[j].name, a->name)) {
                b = &gpu->options[j];
                break;
            }
        }
        if (!b)
            (void)fprintf(stderr, "\nadm_sycl is missing CPU option \"%s\"\n", a->name);
        mu_assert("adm_sycl option table is missing a CPU option", b != NULL);
        mu_assert("adm_sycl option type differs from CPU", a->type == b->type);
        mu_assert("adm_sycl feature-param flag differs from CPU",
                  (a->flags & VMAF_OPT_FLAG_FEATURE_PARAM) ==
                      (b->flags & VMAF_OPT_FLAG_FEATURE_PARAM));
        mu_assert("adm_sycl option alias differs from CPU",
                  (a->alias == NULL) == (b->alias == NULL) &&
                      (a->alias == NULL || !strcmp(a->alias, b->alias)));
    }
    return NULL;
}

/* Never fabricate a feature to make a name resolve: this twin has no AIM
 * device pass, so aim_score / adm3_score must stay out of
 * provided_features[] and fall back to the CPU twin. */
static char *test_adm_sycl_does_not_claim_aim(void)
{
    VmafFeatureExtractor *gpu = vmaf_get_feature_extractor_by_name("adm_sycl");
    mu_assert("adm_sycl extractor must be registered", gpu != NULL);
    mu_assert("adm_sycl must declare provided_features", gpu->provided_features != NULL);
    for (unsigned i = 0; gpu->provided_features[i]; i++) {
        mu_assert("adm_sycl must not claim VMAF_integer_feature_aim_score",
                  strcmp(gpu->provided_features[i], "VMAF_integer_feature_aim_score") != 0);
        mu_assert("adm_sycl must not claim VMAF_integer_feature_adm3_score",
                  strcmp(gpu->provided_features[i], "VMAF_integer_feature_adm3_score") != 0);
    }
    return NULL;
}

/* Structural half of the model-option contract, and the half that is
 * hardware-independent: under the default model's option dict BOTH twins must
 * emit every MODEL_KEYS entry. `run_adm_with_model_opts` asserts that as it
 * reads each score back, so a twin whose option table has drifted fails here
 * rather than in the delta comparison below. Green on Arc A380. */
static char *test_adm_cpu_sycl_model_option_keys(void)
{
    double cpu[NUM_MODEL_KEYS];
    double gpu[NUM_MODEL_KEYS];

    char *msg = run_adm_with_model_opts(false, cpu);
    if (msg)
        return msg;
    return run_adm_with_model_opts(true, gpu);
}

/* Numeric half: csf_mode / min_val / noise_weight / dlm_weight are honoured,
 * not merely declared, so the emitted values must track the CPU reference at
 * places=4 under the default model's option dict.
 *
 * KNOWN RED on Intel Arc A380 with this 256x144 synthetic gradient: the delta
 * lands at ~1.6e-04, the same order as the pre-existing
 * test_adm_cpu_sycl_parity failure at 1.10e-04 on the identical fixture
 * (T-SYCL-ARC-ADM2-PARITY-1.1E-4-2026-09-05 in docs/state.md — reproduced
 * unchanged against origin/master's integer_adm_sycl.cpp, so the option port
 * does not move it). On real 576x324 content the same twin tracks the CPU to
 * <= 1e-06 under this exact option dict; that evidence lives in
 * python/test/gpu_default_model_test.py. The threshold is deliberately NOT
 * loosened to paper over the device gap. */
static char *test_adm_cpu_sycl_model_option_parity(void)
{
    double cpu[NUM_MODEL_KEYS];
    double gpu[NUM_MODEL_KEYS];

    char *msg = run_adm_with_model_opts(false, cpu);
    if (msg)
        return msg;
    msg = run_adm_with_model_opts(true, gpu);
    if (msg)
        return msg;
    if (isnan(gpu[0]))
        return NULL;

    for (unsigned k = 0; k < NUM_MODEL_KEYS; k++) {
        const double delta = fabs(cpu[k] - gpu[k]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nadm model-opt parity FAIL (%s): cpu=%.8f sycl=%.8f "
                          "delta=%.2e tol=%.2e\n",
                          MODEL_KEYS[k], cpu[k], gpu[k], delta, PARITY_TOL);
        }
        mu_assert("adm model-opt CPU vs. SYCL delta exceeds places=4 tolerance (1e-4)",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

static char *test_adm_sycl_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm_sycl");
    mu_assert("adm_sycl extractor must be registered", fex != NULL);
    mu_assert("adm_sycl name matches", !strcmp(fex->name, "adm_sycl"));
    return NULL;
}

static char *test_adm_cpu_sycl_parity(void)
{
    double cpu_adm2 = 0.0;
    double sycl_adm2 = NAN;
    char *msg = run_cpu_adm(&cpu_adm2);
    if (msg)
        return msg;
    msg = run_sycl_adm(&sycl_adm2);
    if (msg)
        return msg;
    if (isnan(sycl_adm2))
        return NULL;
    double delta = fabs(cpu_adm2 - sycl_adm2);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\nadm2 parity FAIL: cpu=%.8f sycl=%.8f delta=%.2e tol=%.2e\n",
                      cpu_adm2, sycl_adm2, delta, PARITY_TOL);
    }
    mu_assert("adm2 CPU vs. SYCL delta exceeds places=4 tolerance (1e-4)", delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_adm_sycl_registered);
    mu_run_test(test_adm_sycl_option_table_mirrors_cpu);
    mu_run_test(test_adm_sycl_does_not_claim_aim);
    /* test_adm_cpu_sycl_parity runs LAST: `mu_run_test` aborts the whole
     * binary on the first failure, and that test is currently red on Intel
     * Arc A380 (delta 1.10e-04 vs the 1e-4 gate, pre-existing on master —
     * T-SYCL-ARC-ADM2-PARITY-1.1E-4-2026-09-05 in docs/state.md). Ordering it
     * after the option-honouring test keeps that known-red assertion from
     * masking a real regression in the new coverage. */
    mu_run_test(test_adm_cpu_sycl_model_option_keys);
    mu_run_test(test_adm_cpu_sycl_model_option_parity);
    mu_run_test(test_adm_cpu_sycl_parity);
    return NULL;
}
