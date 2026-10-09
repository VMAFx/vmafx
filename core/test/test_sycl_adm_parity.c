/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
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
 * ADR-0214 places=4 (1e-4) tolerance, and that under the default model's
 * options every ADM output -- aim and adm3 from the AIM pass included
 * (ADR-1362) -- carries the CPU's bits.
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

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

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
#define MAX_KEYS 32u

/* One sample of plane `p` at (col, row), 8-bit or 16-bit storage. */
static void put_sample(VmafPicture *pic, unsigned p, unsigned col, unsigned row, unsigned v)
{
    uint8_t *line = (uint8_t *)pic->data[p] + ((size_t)row * pic->stride[p]);
    if (pic->bpc == 8u) {
        line[col] = (uint8_t)v;
    } else {
        ((uint16_t *)line)[col] = (uint16_t)v;
    }
}

static int fill_pic(VmafPicture *pic, unsigned salt, unsigned bpc)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, bpc, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    const unsigned mask = (1u << bpc) - 1u;
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            /* Diagonal gradient + salt offset — produces non-trivial
             * detail at every ADM scale so DLM/CSF paths are exercised. */
            put_sample(pic, 0u, col, row, (row * 3u + col * 5u + salt * 17u) & mask);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                put_sample(pic, p, col, row, 1u << (bpc - 1u));
            }
        }
    }
    return 0;
}

static int feed_frame(VmafContext *vmaf, unsigned bpc)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_pic(&ref, 0u, bpc);
    if (err)
        return err;
    err = fill_pic(&dist, 1u, bpc);
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
    err = feed_frame(vmaf, FIXTURE_BPC);
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
    err = feed_frame(vmaf, FIXTURE_BPC);
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
/* The first two keys are the AIM pass's (ADR-1362): the default model reads
 * adm3, so the SYCL twin answers it under the CPU twin's key. */
static const char *const MODEL_KEYS[] = {
    "integer_adm3" MODEL_SUFFIX,       "integer_aim" MODEL_SUFFIX,
    "integer_adm2" MODEL_SUFFIX,       "integer_adm_scale0" MODEL_SUFFIX,
    "integer_adm_scale1" MODEL_SUFFIX, "integer_adm_scale2" MODEL_SUFFIX,
    "integer_adm_scale3" MODEL_SUFFIX,
};
#define NUM_MODEL_KEYS (sizeof(MODEL_KEYS) / sizeof(MODEL_KEYS[0]))

/* One adm (CPU) or adm_sycl context. */
typedef struct AdmRun {
    bool use_sycl;
    VmafSyclState *sycl_state;
    VmafContext *vmaf;
} AdmRun;

/* Open the run's context; `*skipped` without a SYCL device. */
static char *adm_run_open(AdmRun *run, int *skipped)
{
    *skipped = 0;
    if (run->use_sycl) {
        VmafSyclConfiguration sycl_cfg = {.device_index = -1};
        const int sy_err = vmaf_sycl_state_init(&run->sycl_state, sycl_cfg);
        if (sy_err != 0 || run->sycl_state == NULL) {
            (void)fprintf(stderr, "[skip: no SYCL device] ");
            *skipped = 1;
            return NULL;
        }
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    mu_assert("vmaf_init failed", !vmaf_init(&run->vmaf, cfg));
    if (run->use_sycl) {
        mu_assert("vmaf_sycl_import_state failed",
                  !vmaf_sycl_import_state(run->vmaf, run->sycl_state));
    }
    return NULL;
}

/* Register `opts` (NULL for defaults) and, when set, `opts2` as a second
 * registration of the same extractor, which the registry folds into the
 * first (ADR-2795). vmaf_use_feature() takes each dictionary over. */
static char *adm_run_use(AdmRun *run, VmafFeatureDictionary *opts, VmafFeatureDictionary *opts2)
{
    const char *name = run->use_sycl ? "adm_sycl" : "adm";
    const int err = vmaf_use_feature(run->vmaf, name, opts);
    if (err) {
        (void)vmaf_feature_dictionary_free(&opts2);
    }
    mu_assert("vmaf_use_feature failed", !err);
    if (opts2) {
        mu_assert("second vmaf_use_feature failed", !vmaf_use_feature(run->vmaf, name, opts2));
    }
    return NULL;
}

/* Score one `bpc`-bit frame and read `keys` into `out`. */
static char *adm_run_score(AdmRun *run, unsigned bpc, const char *const *keys, size_t count,
                           double *out)
{
    mu_assert("feed_frame failed", !feed_frame(run->vmaf, bpc));
    mu_assert("vmaf_read_pictures(EOS) failed", !vmaf_read_pictures(run->vmaf, NULL, NULL, 0));
    for (size_t k = 0; k < count; k++) {
        const int err = vmaf_feature_score_at_index(run->vmaf, keys[k], &out[k], 0u);
        if (err) {
            (void)fprintf(stderr, "\nmissing feature-name key: %s (%s twin)\n", keys[k],
                          run->use_sycl ? "SYCL" : "CPU");
        }
        mu_assert("feature-name key not emitted", !err);
    }
    return NULL;
}

static char *adm_run_close(AdmRun *run)
{
    if (run->vmaf) {
        mu_assert("vmaf_close failed", !vmaf_close(run->vmaf));
    }
    if (run->sycl_state) {
        vmaf_sycl_state_free(&run->sycl_state);
    }
    return NULL;
}

/* One frame of `bpc` bits on the CPU or the SYCL twin with `opts` and, when
 * set, a second registration with `opts2`; `keys` into `out`. Takes both
 * dictionaries over. Without a SYCL device the SYCL leg is skipped and `out`
 * stays NaN. */
static char *run_adm_keys(bool use_sycl, unsigned bpc, VmafFeatureDictionary *opts,
                          VmafFeatureDictionary *opts2, const char *const *keys, size_t count,
                          double *out)
{
    for (size_t k = 0; k < count; k++)
        out[k] = NAN;
    AdmRun run = {.use_sycl = use_sycl};
    int skipped = 0;
    char *msg = adm_run_open(&run, &skipped);
    if (msg || skipped) {
        (void)vmaf_feature_dictionary_free(&opts);
        (void)vmaf_feature_dictionary_free(&opts2);
        return msg;
    }
    msg = adm_run_use(&run, opts, opts2);
    if (!msg)
        msg = adm_run_score(&run, bpc, keys, count, out);
    char *close_msg = adm_run_close(&run);
    return msg ? msg : close_msg;
}

static char *run_adm_with_model_opts(bool use_sycl, double out[NUM_MODEL_KEYS])
{
    VmafFeatureDictionary *opts = model_opts();
    mu_assert("model-opts: dictionary build failed", opts != NULL);
    return run_adm_keys(use_sycl, FIXTURE_BPC, opts, NULL, MODEL_KEYS, NUM_MODEL_KEYS, out);
}

/* Every option the CPU table declares must also exist, with the same alias,
 * type and feature-param flag, in the SYCL table — otherwise the emitted
 * feature-name key diverges. */
// NOLINTNEXTLINE(readability-function-size): test scaffolding (ADR-0141 / ADR-0278) — the body walks the whole allocate / fill / run-CPU / run-SYCL / compare / free sequence in one place so a parity failure points at the exact stage that diverged; splitting it hides which assertion fired.
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

static bool provides(const VmafFeatureExtractor *fex, const char *feature)
{
    for (unsigned i = 0; fex->provided_features[i]; i++) {
        if (!strcmp(fex->provided_features[i], feature))
            return true;
    }
    return false;
}

/* The AIM pass runs on the device (ADR-1362), so the twin claims both
 * features: a model asking for adm3 under --backend sycl resolves to adm_sycl
 * rather than to the CPU twin (ADR-0530 fallback), and the default model's
 * whole ADM branch stays on the device. */
static char *test_adm_sycl_claims_aim(void)
{
    VmafFeatureExtractor *gpu = vmaf_get_feature_extractor_by_name("adm_sycl");
    mu_assert("adm_sycl extractor must be registered", gpu != NULL);
    mu_assert("adm_sycl must declare provided_features", gpu->provided_features != NULL);
    mu_assert("adm_sycl must claim VMAF_integer_feature_aim_score",
              provides(gpu, "VMAF_integer_feature_aim_score"));
    mu_assert("adm_sycl must claim VMAF_integer_feature_adm3_score",
              provides(gpu, "VMAF_integer_feature_adm3_score"));
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

/* Bit-for-bit equality of two scores (compares the IEEE-754 bit patterns, not
 * the object representations, which tidy rejects for double). */
static bool same_bits(double a, double b)
{
    uint64_t ua = 0;
    uint64_t ub = 0;
    memcpy(&ua, &a, sizeof(ua));
    memcpy(&ub, &b, sizeof(ub));
    return ua == ub;
}

/* `cpu` and `gpu` equal key by key, bit for bit; a NaN first SYCL value
 * means the SYCL leg was skipped. */
static char *compare_exact(const char *what, const char *const *keys, size_t count,
                           const double *cpu, const double *gpu)
{
    if (isnan(gpu[0]))
        return NULL;
    unsigned mismatches = 0u;
    for (size_t k = 0; k < count; k++) {
        if (!same_bits(cpu[k], gpu[k])) {
            (void)fprintf(stderr, "\n%s: %s not bit-exact: cpu=%.17g sycl=%.17g\n", what, keys[k],
                          cpu[k], gpu[k]);
            mismatches++;
        }
    }
    mu_assert("ADM: SYCL differs from the CPU bits", mismatches == 0u);
    return NULL;
}

/* One `bpc`-bit frame with `make_opts()` (NULL: defaults) on the CPU and on
 * the SYCL twin; every key must carry the CPU's bits. */
static char *check_exact(const char *what, unsigned bpc, VmafFeatureDictionary *(*make_opts)(void),
                         const char *const *keys, size_t count)
{
    double cpu[MAX_KEYS];
    double gpu[MAX_KEYS];
    mu_assert("check_exact: too many keys", count <= MAX_KEYS);
    char *msg = run_adm_keys(false, bpc, make_opts ? make_opts() : NULL, NULL, keys, count, cpu);
    if (!msg)
        msg = run_adm_keys(true, bpc, make_opts ? make_opts() : NULL, NULL, keys, count, gpu);
    return msg ? msg : compare_exact(what, keys, count, cpu, gpu);
}

/* ADR-1362 numerical contract: the device accumulators are bit-exact with the
 * CPU's and the host finalises them in the CPU's own float arithmetic, so
 * every emitted ADM score must carry the CPU's bits, not merely agree to
 * places=4. */
static char *test_adm_cpu_sycl_bit_exact(void)
{
    return check_exact("model options", FIXTURE_BPC, model_opts, MODEL_KEYS, NUM_MODEL_KEYS);
}

#define ADM_SCORE_KEYS(suffix)                                                                     \
    "integer_adm2" suffix, "integer_aim" suffix, "integer_adm3" suffix,                            \
        "integer_adm_scale0" suffix, "integer_adm_scale1" suffix, "integer_adm_scale2" suffix,     \
        "integer_adm_scale3" suffix
#define ADM_DEBUG_KEYS                                                                             \
    "integer_adm", "integer_adm_num", "integer_adm_den", "integer_adm_num_scale0",                 \
        "integer_adm_den_scale0", "integer_adm_num_scale1", "integer_adm_den_scale1",              \
        "integer_adm_num_scale2", "integer_adm_den_scale2", "integer_adm_num_scale3",              \
        "integer_adm_den_scale3"

/* ADR-2795: one context at two viewing distances. The first distance keeps
 * its names and, with `debug`, the debug scores; the second files its seven
 * under the names `adm_norm_view_dist=5` gives them. */
static VmafFeatureDictionary *two_view_opts(void)
{
    VmafFeatureDictionary *d = NULL;
    if (vmaf_feature_dictionary_set(&d, "debug", "true") ||
        vmaf_feature_dictionary_set(&d, "adm_norm_view_dist_extra", "5")) {
        (void)vmaf_feature_dictionary_free(&d);
    }
    return d;
}

static const char *const TWO_VIEW_KEYS[] = {
    "VMAF_integer_feature_adm2_score",
    "VMAF_integer_feature_aim_score",
    "VMAF_integer_feature_adm3_score",
    "integer_adm_scale0",
    "integer_adm_scale1",
    "integer_adm_scale2",
    "integer_adm_scale3",
    ADM_DEBUG_KEYS,
    ADM_SCORE_KEYS("_nvd_5"),
};
#define NUM_TWO_VIEW_KEYS (sizeof(TWO_VIEW_KEYS) / sizeof(TWO_VIEW_KEYS[0]))

/* The model options with `nvd_key` = `nvd`. */
static VmafFeatureDictionary *model_opts_at(const char *nvd_key, const char *nvd)
{
    VmafFeatureDictionary *d = model_opts();
    if (d && vmaf_feature_dictionary_set(&d, nvd_key, nvd)) {
        (void)vmaf_feature_dictionary_free(&d);
    }
    return d;
}

/* The model options at 3H, then at 5H: the vmaf_v1.0.16_3d0h / _5d0h pair. */
static VmafFeatureDictionary *two_view_model_opts(void)
{
    return model_opts_at("adm_norm_view_dist_extra", "5");
}

static const char *const TWO_VIEW_MODEL_KEYS[] = {
    ADM_SCORE_KEYS(MODEL_SUFFIX),
    ADM_SCORE_KEYS(MODEL_SUFFIX "_nvd_5"),
};
#define NUM_TWO_VIEW_MODEL_KEYS (sizeof(TWO_VIEW_MODEL_KEYS) / sizeof(TWO_VIEW_MODEL_KEYS[0]))

/* ADR-2795: a second viewing distance on the SYCL twin returns the CPU's
 * scores for both distances, at 8 and 10 bits and under the model options. */
static char *test_adm_two_views_exact(void)
{
    char *msg = check_exact("two views", 8u, two_view_opts, TWO_VIEW_KEYS, NUM_TWO_VIEW_KEYS);
    if (!msg) {
        msg = check_exact("two views 10-bit", 10u, two_view_opts, TWO_VIEW_KEYS, NUM_TWO_VIEW_KEYS);
    }
    if (!msg) {
        msg = check_exact("two views model options", 8u, two_view_model_opts, TWO_VIEW_MODEL_KEYS,
                          NUM_TWO_VIEW_MODEL_KEYS);
    }
    return msg;
}

/* ADR-2795: the SYCL twin registered at 3H and at 5H (the registry folds the
 * second into the first, as two models do) scores as one CPU context with
 * both distances. */
static char *test_adm_merged_registrations_exact(void)
{
    double cpu[MAX_KEYS];
    double gpu[MAX_KEYS];
    char *msg = run_adm_keys(false, 8u, two_view_model_opts(), NULL, TWO_VIEW_MODEL_KEYS,
                             NUM_TWO_VIEW_MODEL_KEYS, cpu);
    if (!msg) {
        msg = run_adm_keys(true, 8u, model_opts(), model_opts_at("adm_norm_view_dist", "5"),
                           TWO_VIEW_MODEL_KEYS, NUM_TWO_VIEW_MODEL_KEYS, gpu);
    }
    return msg ? msg :
                 compare_exact("merged registrations", TWO_VIEW_MODEL_KEYS, NUM_TWO_VIEW_MODEL_KEYS,
                               cpu, gpu);
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

/* The bit-exact cases: the model options and both viewing distances (ADR-2795). */
static char *run_exact_cases(void)
{
    mu_run_test(test_adm_cpu_sycl_bit_exact);
    mu_run_test(test_adm_two_views_exact);
    mu_run_test(test_adm_merged_registrations_exact);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_adm_sycl_registered);
    mu_run_test(test_adm_sycl_option_table_mirrors_cpu);
    mu_run_test(test_adm_sycl_claims_aim);
    /* test_adm_cpu_sycl_parity runs LAST: `mu_run_test` aborts the whole
     * binary on the first failure, and that test is currently red on Intel
     * Arc A380 (delta 1.10e-04 vs the 1e-4 gate, pre-existing on master —
     * T-SYCL-ARC-ADM2-PARITY-1.1E-4-2026-09-05 in docs/state.md). Ordering it
     * after the option-honouring test keeps that known-red assertion from
     * masking a real regression in the new coverage. */
    mu_run_test(test_adm_cpu_sycl_model_option_keys);
    mu_run_test(test_adm_cpu_sycl_model_option_parity);
    mu_assert_msg(run_exact_cases());
    mu_run_test(test_adm_cpu_sycl_parity);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
