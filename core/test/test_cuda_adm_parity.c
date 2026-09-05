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
 * Round-2 GPU-kernel coverage gap-fill — adm2 / adm3 CPU vs. CUDA parity test.
 *
 * The ADM (Additive Detail Metric) is computed by integer_adm.c (CPU scalar)
 * and integer_adm_cuda.c + integer_adm/*.cu (CUDA kernel set: adm_dwt2,
 * adm_decouple, adm_csf, adm_csf_den, adm_cm). It is a load-bearing component
 * of the libvmaf-2.x.x default model — a regression in the DWT2 stage or the
 * CSF normaliser would silently bias the VMAF score across CHUG re-extracts.
 *
 * No cross-backend assertion existed before this test for the ADM kernel set;
 * round-1 (PR #351) covered psnr_cuda + ciede_cuda only.
 *
 * Skip behaviour: if vmaf_cuda_state_init() fails (no CUDA driver / no device)
 * the test emits "[skip: no CUDA device]" and passes.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

/* Fixture geometry — large enough for the 4-scale ADM DWT pyramid. */
#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u

/* ADR-0214 cross-backend gate (places=4 → 1e-4). */
#define PARITY_TOL 1e-4

/* The two top-level ADM features both backends must emit. */
static const char *const ADM_FEATURES[] = {
    "VMAF_integer_feature_adm2_score",
    "VMAF_integer_feature_adm3_score",
};
#define NUM_ADM_FEATURES 2u

static int fill_ref(VmafPicture *pic)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row * 3u + col * 2u) & 0xFFu);
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

static int fill_dist(VmafPicture *pic)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            /* Same base ramp as ref + a small additive perturbation so ADM
             * produces non-trivial scores without saturating to 1.0. */
            const int v = (int)((row * 3u + col * 2u) & 0xFFu) + 9;
            y[row * pic->stride[0] + col] = (uint8_t)(v > 255 ? 255 : v);
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

static char *feed_one_frame(VmafContext *vmaf)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_ref(&ref);
    mu_assert("fill_ref failed", !err);
    err = fill_dist(&dist);
    mu_assert("fill_dist failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    mu_assert("vmaf_read_pictures failed", !err);
    return NULL;
}

static char *run_cpu_adm(double scores_out[NUM_ADM_FEATURES])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "adm", NULL);
    mu_assert("CPU: vmaf_use_feature(adm) failed", !err);

    char *msg = feed_one_frame(vmaf);
    if (msg)
        return msg;
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    for (unsigned k = 0; k < NUM_ADM_FEATURES; k++) {
        err = vmaf_feature_score_at_index(vmaf, ADM_FEATURES[k], &scores_out[k], 0u);
        mu_assert("CPU: vmaf_feature_score_at_index(adm) failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_cuda_adm(double scores_out[NUM_ADM_FEATURES])
{
    for (unsigned k = 0; k < NUM_ADM_FEATURES; k++)
        scores_out[k] = NAN;

    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    int err = vmaf_cuda_state_init(&cu_state, cuda_cfg);
    if (err != 0 || cu_state == NULL) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CUDA: vmaf_init failed", !err);

    err = vmaf_cuda_import_state(vmaf, cu_state);
    mu_assert("CUDA: vmaf_cuda_import_state failed", !err);

    err = vmaf_use_feature(vmaf, "adm_cuda", NULL);
    mu_assert("CUDA: vmaf_use_feature(adm_cuda) failed", !err);

    char *msg = feed_one_frame(vmaf);
    if (msg)
        return msg;
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CUDA: vmaf_read_pictures(EOS) failed", !err);

    for (unsigned k = 0; k < NUM_ADM_FEATURES; k++) {
        err = vmaf_feature_score_at_index(vmaf, ADM_FEATURES[k], &scores_out[k], 0u);
        mu_assert("CUDA: vmaf_feature_score_at_index(adm) failed", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}

/* The default model `vmaf_v1.0.16_3d0h` asks integer ADM for
 * VMAF_integer_feature_adm3_score with exactly these five options. Because
 * every one of them is a VMAF_OPT_FLAG_FEATURE_PARAM and non-default, the
 * feature-name key both twins must emit is the fully-suffixed form below —
 * see MODEL_KEYS. A twin whose option table is missing one entry emits a
 * shorter key and the model lookup misses. */
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
    if (vmaf_feature_dictionary_set(&d, "adm_p_norm", "2.0"))
        return NULL;
    return d;
}

#define MODEL_SUFFIX "_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02_apn_2"
static const char *const MODEL_KEYS[] = {
    "integer_adm2" MODEL_SUFFIX,       "integer_aim" MODEL_SUFFIX,
    "integer_adm3" MODEL_SUFFIX,       "integer_adm_scale0" MODEL_SUFFIX,
    "integer_adm_scale1" MODEL_SUFFIX, "integer_adm_scale2" MODEL_SUFFIX,
    "integer_adm_scale3" MODEL_SUFFIX,
};
#define NUM_MODEL_KEYS (sizeof(MODEL_KEYS) / sizeof(MODEL_KEYS[0]))

/* Run one extractor under model_opts() and read every MODEL_KEYS entry back.
 * `use_cuda` selects the twin; on a machine without a CUDA device the CUDA leg
 * reports a skip and leaves the scores NaN. */
static char *run_adm_with_model_opts(bool use_cuda, double out[NUM_MODEL_KEYS])
{
    for (unsigned k = 0; k < NUM_MODEL_KEYS; k++)
        out[k] = NAN;

    VmafCudaState *cu_state = NULL;
    if (use_cuda) {
        VmafCudaConfiguration cuda_cfg = {0};
        const int cu_err = vmaf_cuda_state_init(&cu_state, cuda_cfg);
        if (cu_err != 0 || cu_state == NULL) {
            (void)fprintf(stderr, "[skip: no CUDA device] ");
            return NULL;
        }
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("model-opts: vmaf_init failed", !err);
    if (use_cuda) {
        err = vmaf_cuda_import_state(vmaf, cu_state);
        mu_assert("model-opts: vmaf_cuda_import_state failed", !err);
    }

    VmafFeatureDictionary *opts = model_opts();
    mu_assert("model-opts: dictionary build failed", opts != NULL);
    err = vmaf_use_feature(vmaf, use_cuda ? "adm_cuda" : "adm", opts);
    mu_assert("model-opts: vmaf_use_feature failed", !err);

    char *msg = feed_one_frame(vmaf);
    if (msg)
        return msg;
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("model-opts: vmaf_read_pictures(EOS) failed", !err);

    for (unsigned k = 0; k < NUM_MODEL_KEYS; k++) {
        err = vmaf_feature_score_at_index(vmaf, MODEL_KEYS[k], &out[k], 0u);
        if (err) {
            (void)fprintf(stderr, "\nmissing feature-name key: %s (%s twin)\n", MODEL_KEYS[k],
                          use_cuda ? "CUDA" : "CPU");
        }
        mu_assert("model-opts: feature-name key not emitted", !err);
    }

    err = vmaf_close(vmaf);
    mu_assert("model-opts: vmaf_close failed", !err);
    if (use_cuda) {
        err = vmaf_cuda_state_free(cu_state);
        mu_assert("model-opts: vmaf_cuda_state_free failed", !err);
    }
    return NULL;
}

/* Every option the CPU table declares must also exist, with the same alias,
 * type, default, bounds and feature-param flag, in the CUDA table — otherwise
 * the emitted feature-name key diverges (feature_name.cpp builds it from the
 * extractor's own table). */
static char *test_adm_cuda_option_table_mirrors_cpu(void)
{
    VmafFeatureExtractor *cpu = vmaf_get_feature_extractor_by_name("adm");
    VmafFeatureExtractor *gpu = vmaf_get_feature_extractor_by_name("adm_cuda");
    mu_assert("adm extractor must be registered", cpu != NULL);
    mu_assert("adm_cuda extractor must be registered", gpu != NULL);
    mu_assert("adm must declare options", cpu->options != NULL);
    mu_assert("adm_cuda must declare options", gpu->options != NULL);

    for (unsigned i = 0; cpu->options[i].name; i++) {
        const VmafOption *a = &cpu->options[i];
        const VmafOption *b = NULL;
        for (unsigned j = 0; gpu->options[j].name; j++) {
            if (!strcmp(gpu->options[j].name, a->name)) {
                b = &gpu->options[j];
                break;
            }
        }
        if (!b)
            (void)fprintf(stderr, "\nadm_cuda is missing CPU option \"%s\"\n", a->name);
        mu_assert("adm_cuda option table is missing a CPU option", b != NULL);
        mu_assert("adm_cuda option type differs from CPU", a->type == b->type);
        mu_assert("adm_cuda feature-param flag differs from CPU",
                  (a->flags & VMAF_OPT_FLAG_FEATURE_PARAM) ==
                      (b->flags & VMAF_OPT_FLAG_FEATURE_PARAM));
        mu_assert("adm_cuda option alias differs from CPU",
                  (a->alias == NULL) == (b->alias == NULL) &&
                      (a->alias == NULL || !strcmp(a->alias, b->alias)));
    }
    return NULL;
}

/* csf_mode / p_norm / dlm_weight / min_val / noise_weight are honoured by the
 * CUDA twin, not merely declared: the seven emitted values must match the CPU
 * reference at places=4 under the default model's option dict. */
static char *test_adm_cpu_cuda_model_option_parity(void)
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
                          "\nadm model-opt parity FAIL (%s): cpu=%.8f cuda=%.8f "
                          "delta=%.2e tol=%.2e\n",
                          MODEL_KEYS[k], cpu[k], gpu[k], delta, PARITY_TOL);
        }
        mu_assert("adm model-opt CPU vs. CUDA delta exceeds places=4 tolerance (1e-4)",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

static char *test_adm_cuda_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm_cuda");
    mu_assert("adm_cuda extractor must be registered", fex != NULL);
    mu_assert("adm_cuda name matches", !strcmp(fex->name, "adm_cuda"));
    return NULL;
}

static char *test_adm_cpu_cuda_parity(void)
{
    double cpu[NUM_ADM_FEATURES] = {0.0, 0.0};
    double gpu[NUM_ADM_FEATURES] = {NAN, NAN};

    char *msg = run_cpu_adm(cpu);
    if (msg)
        return msg;
    msg = run_cuda_adm(gpu);
    if (msg)
        return msg;
    if (isnan(gpu[0]))
        return NULL;

    for (unsigned k = 0; k < NUM_ADM_FEATURES; k++) {
        const double delta = fabs(cpu[k] - gpu[k]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nadm parity FAIL (%s): cpu=%.8f cuda=%.8f delta=%.2e tol=%.2e\n",
                          ADM_FEATURES[k], cpu[k], gpu[k], delta, PARITY_TOL);
        }
        mu_assert("adm CPU vs. CUDA delta exceeds places=4 tolerance (1e-4)", delta <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_adm_cuda_registered);
    mu_run_test(test_adm_cuda_option_table_mirrors_cpu);
    mu_run_test(test_adm_cpu_cuda_parity);
    mu_run_test(test_adm_cpu_cuda_model_option_parity);
    return NULL;
}
