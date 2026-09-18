/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Round-2 GPU-kernel coverage gap-fill — adm2 / adm3 CPU vs. CUDA parity test.
 *
 * The ADM (Additive Detail Metric) is computed by integer_adm.c (CPU scalar)
 * and integer_adm_cuda.c plus the .cu kernels under integer_adm/ (adm_dwt2,
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

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

/* Fixture geometry — large enough for the 4-scale ADM DWT pyramid. */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
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

/* One scoring run of the CPU `adm` or the CUDA `adm_cuda` extractor. */
typedef struct {
    bool use_cuda;
    VmafContext *vmaf;
    VmafCudaState *cu_state;
} AdmRun;

/* Create the context (and the CUDA state for the CUDA twin). `*skipped` is set
 * when the CUDA leg finds no device. */
static char *adm_run_init(AdmRun *run, int *skipped)
{
    if (run->use_cuda) {
        VmafCudaConfiguration cuda_cfg = {0};
        if (vmaf_cuda_state_init(&run->cu_state, cuda_cfg) != 0 || run->cu_state == NULL) {
            (void)fprintf(stderr, "[skip: no CUDA device] ");
            mu_skipped = 1;
            *skipped = 1;
            return NULL;
        }
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    mu_assert("vmaf_init failed", !vmaf_init(&run->vmaf, cfg));
    if (run->use_cuda) {
        mu_assert("vmaf_cuda_import_state failed",
                  !vmaf_cuda_import_state(run->vmaf, run->cu_state));
    }
    return NULL;
}

/* Open the run with `opts` (NULL for defaults), which it always takes over:
 * vmaf_use_feature() consumes it, and it is freed here when the run never gets
 * that far. */
static char *adm_run_open(AdmRun *run, VmafFeatureDictionary *opts, int *skipped)
{
    *skipped = 0;
    char *msg = adm_run_init(run, skipped);
    if (msg || *skipped) {
        (void)vmaf_feature_dictionary_free(&opts);
        return msg;
    }
    mu_assert("vmaf_use_feature failed",
              !vmaf_use_feature(run->vmaf, run->use_cuda ? "adm_cuda" : "adm", opts));
    return NULL;
}

/* Feed the fixture frame, flush, and read every key in `keys`. */
static char *adm_run_score(const AdmRun *run, const char *const *keys, size_t count, double *out)
{
    char *msg = feed_one_frame(run->vmaf);
    if (msg)
        return msg;
    mu_assert("vmaf_read_pictures(EOS) failed", !vmaf_read_pictures(run->vmaf, NULL, NULL, 0));
    for (size_t k = 0; k < count; k++) {
        if (vmaf_feature_score_at_index(run->vmaf, keys[k], &out[k], 0u)) {
            (void)fprintf(stderr, "\nmissing feature-name key: %s (%s twin)\n", keys[k],
                          run->use_cuda ? "CUDA" : "CPU");
            return "ADM feature-name key not emitted";
        }
    }
    return NULL;
}

static char *adm_run_close(AdmRun *run)
{
    if (run->vmaf)
        mu_assert("vmaf_close failed", !vmaf_close(run->vmaf));
    if (run->cu_state)
        mu_assert("vmaf_cuda_state_free failed", !vmaf_cuda_state_free(run->cu_state));
    return NULL;
}

/* Score one frame on the CPU or CUDA twin and read `keys` into `out`. Without a
 * CUDA device the CUDA leg is skipped and `out` stays NaN. */
static char *run_adm(bool use_cuda, VmafFeatureDictionary *opts, const char *const *keys,
                     size_t count, double *out)
{
    for (size_t k = 0; k < count; k++)
        out[k] = NAN;
    AdmRun run = {.use_cuda = use_cuda};
    int skipped = 0;
    char *msg = adm_run_open(&run, opts, &skipped);
    if (!msg && !skipped)
        msg = adm_run_score(&run, keys, count, out);
    char *close_msg = adm_run_close(&run);
    return msg ? msg : close_msg;
}

/* Compare the CPU and CUDA scores of `keys`; a skipped CUDA leg passes. */
static char *check_parity(const char *what, VmafFeatureDictionary *(*make_opts)(void),
                          const char *const *keys, size_t count)
{
    double cpu[16];
    double gpu[16];
    mu_assert("check_parity: too many keys", count <= 16);
    char *msg = run_adm(false, make_opts ? make_opts() : NULL, keys, count, cpu);
    if (msg)
        return msg;
    msg = run_adm(true, make_opts ? make_opts() : NULL, keys, count, gpu);
    if (msg || isnan(gpu[0]))
        return msg;
    for (size_t k = 0; k < count; k++) {
        const double delta = fabs(cpu[k] - gpu[k]);
        if (!(delta <= PARITY_TOL)) {
            (void)fprintf(stderr, "\n%s parity FAIL (%s): cpu=%.8f cuda=%.8f delta=%.2e tol=%.2e\n",
                          what, keys[k], cpu[k], gpu[k], delta, PARITY_TOL);
            return "adm CPU vs. CUDA delta exceeds places=4 tolerance (1e-4)";
        }
    }
    return NULL;
}

/* The default model `vmaf_v1.0.16_3d0h` asks integer ADM for
 * VMAF_integer_feature_adm3_score with exactly these five options. Because
 * every one of them is a VMAF_OPT_FLAG_FEATURE_PARAM and non-default, the
 * feature-name key both twins must emit is the fully-suffixed form below --
 * see MODEL_KEYS. A twin whose option table is missing one entry emits a
 * shorter key and the model lookup misses. */
static VmafFeatureDictionary *model_opts(void)
{
    static const char *const pairs[][2] = {
        {"adm_csf_mode", "2"},  {"adm_dlm_weight", "0.7"},    {"adm_enhn_gain_limit", "1.0"},
        {"adm_min_val", "0.5"}, {"adm_noise_weight", "0.02"}, {"adm_p_norm", "2.0"},
    };
    VmafFeatureDictionary *d = NULL;
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        if (vmaf_feature_dictionary_set(&d, pairs[i][0], pairs[i][1])) {
            (void)vmaf_feature_dictionary_free(&d);
            return NULL;
        }
    }
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

static const VmafOption *find_option(const VmafOption *table, const char *name)
{
    for (unsigned j = 0; table[j].name; j++) {
        if (!strcmp(table[j].name, name))
            return &table[j];
    }
    return NULL;
}

/* 1 when `b` declares `a` identically for feature-name purposes. */
static int option_mirrors(const VmafOption *a, const VmafOption *b)
{
    const int same_alias = (a->alias == NULL) == (b->alias == NULL) &&
                           (a->alias == NULL || !strcmp(a->alias, b->alias));
    return a->type == b->type &&
           (a->flags & VMAF_OPT_FLAG_FEATURE_PARAM) == (b->flags & VMAF_OPT_FLAG_FEATURE_PARAM) &&
           same_alias;
}

/* Every option the CPU table declares must also exist, with the same alias,
 * type and feature-param flag, in the CUDA table -- otherwise the emitted
 * feature-name key diverges (feature_name.cpp builds it from the extractor's
 * own table). */
static char *test_adm_cuda_option_table_mirrors_cpu(void)
{
    VmafFeatureExtractor *cpu = vmaf_get_feature_extractor_by_name("adm");
    VmafFeatureExtractor *gpu = vmaf_get_feature_extractor_by_name("adm_cuda");
    mu_assert("adm extractor must be registered", cpu != NULL);
    mu_assert("adm_cuda extractor must be registered", gpu != NULL);
    mu_assert("adm must declare options", cpu->options != NULL);
    mu_assert("adm_cuda must declare options", gpu->options != NULL);

    for (unsigned i = 0; cpu->options[i].name; i++) {
        const VmafOption *b = find_option(gpu->options, cpu->options[i].name);
        if (!b || !option_mirrors(&cpu->options[i], b)) {
            (void)fprintf(stderr, "\nadm_cuda does not mirror CPU option \"%s\"\n",
                          cpu->options[i].name);
            return "adm_cuda option table does not mirror the CPU table";
        }
    }
    return NULL;
}

/* csf_mode / p_norm / dlm_weight / min_val / noise_weight are honoured by the
 * CUDA twin, not merely declared: the seven emitted values must match the CPU
 * reference at places=4 under the default model's option dict. */
static char *test_adm_cpu_cuda_model_option_parity(void)
{
    return check_parity("adm model-opt", model_opts, MODEL_KEYS, NUM_MODEL_KEYS);
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
    return check_parity("adm", NULL, ADM_FEATURES, NUM_ADM_FEATURES);
}

char *run_tests(void)
{
    mu_run_test(test_adm_cuda_registered);
    mu_run_test(test_adm_cuda_option_table_mirrors_cpu);
    mu_run_test(test_adm_cpu_cuda_parity);
    mu_run_test(test_adm_cpu_cuda_model_option_parity);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
