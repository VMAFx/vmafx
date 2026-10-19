/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * adm CPU vs. CUDA parity (round-2 GPU-kernel coverage; exact since ADR-1416).
 *
 * The ADM (Additive Detail Metric) is computed by integer_adm.c (CPU) and by
 * integer_adm_cuda.c plus the .cu kernels under integer_adm/ (adm_dwt2,
 * adm_csf, adm_csf_den, adm_cm). It is a load-bearing component of the
 * default model: a regression in the DWT stage or the CSF normaliser would
 * silently bias the VMAF score.
 *
 * Since ADR-1416 the twin takes its CSF weights, its rounding shifts and its
 * float conclusion from the CPU's own routines (integer_adm_kernels.h) and
 * rounds the denominator once per row as the CPU does, so this test asserts
 * equality, not a tolerance: every output of every case has the CPU's bits,
 * the per-scale numerators and denominators of `debug=true` included.
 *
 * Before ADR-1416 the host kept a copy of dwt_quant_step() that evaluated the
 * CSF exponent in float where the CPU evaluates it in double (weights 1 to 3
 * units in the last place apart), and the denominator kernels rounded each
 * warp of a row on its own. Every exact case below fails on that twin.
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

#define MAX_KEYS 32u

/* One frame geometry of a case. */
typedef struct Fixture {
    unsigned w;
    unsigned h;
    unsigned bpc;
    bool sparse; /* a flat frame with isolated one-level dots instead of texture */
    bool graded; /* isolated near-full-scale patches, the distorted frame at 60-99% of them */
} Fixture;

/* Deterministic position hash for the sparse fixture. */
static unsigned position_hash(unsigned row, unsigned col, unsigned salt)
{
    unsigned x = row * 73856093u ^ col * 19349663u ^ salt * 83492791u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return x;
}

/* Luma of the textured fixture: a wrapping ramp with texture for the
 * reference; the distorted frame adds a position-dependent error, so every
 * scale has detail to lose and to mask. */
static unsigned textured_luma(unsigned row, unsigned col, bool distorted)
{
    unsigned v = ((row * 3u + col * 2u) & 0xFFu) ^ (((row >> 2) * (col >> 3)) & 0x1Fu);
    if (distorted)
        v += 9u + ((row * 5u + col * 7u) % 11u);
    return v;
}

/* Luma of the sparse fixture: mid grey with one sample in 200 raised by a
 * level, and a distorted frame that raises half of the samples by one more.
 * The reference bands are then almost empty, the denominator accumulators
 * are small integers, and where their rows are rounded shows in the score. */
static unsigned sparse_luma(unsigned row, unsigned col, bool distorted)
{
    unsigned v = 128u + (position_hash(row, col, 1u) % 200u == 0u ? 1u : 0u);
    if (distorted)
        v += position_hash(row, col, 2u) & 1u;
    return v;
}

/* Luma of the graded fixture: mid grey with isolated 4x4 patches of
 * near-full-scale detail, about one cell of the 16-sample grid in 14, whose
 * signs follow the signs of the DWT high-pass taps (-, -, +, -) in both
 * directions, so one scale-0 band sample of a patch reaches 17000 to 22000.
 * The distorted frame keeps 60 to 99 percent of each patch. The scale-0 ratio
 * k = t / o then stays below 1 at reference coefficients above 16566, where
 * the reciprocal 2^30 / o taken as an fp32 quotient (the twins' old form)
 * changes the restored sample for a few band values; the flat frame around
 * the patches leaves too little else for that to vanish in the score. The
 * patches are chosen so that this happens on the 224x224 frame. */
static unsigned graded_luma(unsigned row, unsigned col, bool distorted)
{
    static const int sign4[4] = {-1, -1, 1, -1};
    if (row < 9u || col < 9u || row >= 8u + 16u * 13u || col >= 8u + 16u * 13u) {
        return 128u;
    }
    const unsigned cell_r = (row - 8u) / 16u;
    const unsigned cell_c = (col - 8u) / 16u;
    const unsigned a = ((row - 8u) % 16u) - 1u;
    const unsigned b = ((col - 8u) % 16u) - 1u;
    if (a >= 4u || b >= 4u || position_hash(cell_r, cell_c, 90u) % 14u != 0u) {
        return 128u;
    }
    const int pct = 60 + (int)(position_hash(cell_r, cell_c, 91u) % 40u);
    const int amp = 105 + (int)(position_hash(row, col, 92u) % 23u);
    const int p = sign4[a] * sign4[b] * amp;
    return (unsigned)(128 + (distorted ? (p * pct) / 100 : p));
}

static void put_sample(VmafPicture *pic, unsigned plane, unsigned row, unsigned col, unsigned v)
{
    const unsigned peak = (1u << pic->bpc) - 1u;
    uint8_t *line = (uint8_t *)pic->data[plane] + (size_t)row * (size_t)pic->stride[plane];
    if (pic->bpc <= 8u) {
        line[col] = (uint8_t)(v > peak ? peak : v);
    } else {
        ((uint16_t *)line)[col] = (uint16_t)(v > peak ? peak : v);
    }
}

/* Luma of one sample of the fixture `fx`. */
static unsigned fixture_luma(const Fixture *fx, unsigned row, unsigned col, bool distorted)
{
    if (fx->graded) {
        return graded_luma(row, col, distorted);
    }
    if (fx->sparse) {
        return sparse_luma(row, col, distorted);
    }
    return textured_luma(row, col, distorted);
}

static int fill_picture(VmafPicture *pic, const Fixture *fx, bool distorted)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, fx->bpc, fx->w, fx->h);
    if (err)
        return err;
    const unsigned gain = 1u << (fx->bpc - 8u);
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const unsigned v = fixture_luma(fx, row, col, distorted);
            put_sample(pic, 0u, row, col, v * gain);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++)
                put_sample(pic, p, row, col, 128u * gain);
        }
    }
    return 0;
}

static char *feed_one_frame(VmafContext *vmaf, const Fixture *fx)
{
    VmafPicture ref;
    VmafPicture dist;
    mu_assert("fill reference failed", !fill_picture(&ref, fx, false));
    mu_assert("fill distorted failed", !fill_picture(&dist, fx, true));
    mu_assert("vmaf_read_pictures failed", !vmaf_read_pictures(vmaf, &ref, &dist, 0u));
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

/* Open the run with `opts` (NULL for defaults) and, when `opts2` is set, a
 * second registration of the same extractor with it. The run always takes
 * both over: vmaf_use_feature() consumes them, and they are freed here when
 * the run never gets that far. */
static char *adm_run_open(AdmRun *run, VmafFeatureDictionary *opts, VmafFeatureDictionary *opts2,
                          int *skipped)
{
    *skipped = 0;
    char *msg = adm_run_init(run, skipped);
    if (msg || *skipped) {
        (void)vmaf_feature_dictionary_free(&opts);
        (void)vmaf_feature_dictionary_free(&opts2);
        return msg;
    }
    const char *name = run->use_cuda ? "adm_cuda" : "adm";
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

/* Feed the fixture frame, flush, and read every key in `keys`. */
static char *adm_run_score(const AdmRun *run, const Fixture *fx, const char *const *keys,
                           size_t count, double *out)
{
    char *msg = feed_one_frame(run->vmaf, fx);
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
static char *run_adm2(bool use_cuda, const Fixture *fx, VmafFeatureDictionary *opts,
                      VmafFeatureDictionary *opts2, const char *const *keys, size_t count,
                      double *out)
{
    for (size_t k = 0; k < count; k++)
        out[k] = NAN;
    AdmRun run = {.use_cuda = use_cuda};
    int skipped = 0;
    char *msg = adm_run_open(&run, opts, opts2, &skipped);
    if (!msg && !skipped)
        msg = adm_run_score(&run, fx, keys, count, out);
    char *close_msg = adm_run_close(&run);
    return msg ? msg : close_msg;
}

static char *run_adm(bool use_cuda, const Fixture *fx, VmafFeatureDictionary *opts,
                     const char *const *keys, size_t count, double *out)
{
    return run_adm2(use_cuda, fx, opts, NULL, keys, count, out);
}

/* Every key of the case equal on the CPU and the CUDA twin, bit for bit; a
 * skipped CUDA leg passes. */
/* `cpu` and `gpu` equal key by key; a NaN first CUDA value means skipped. */
static char *compare_runs(const char *what, const Fixture *fx, const char *const *keys,
                          size_t count, const double *cpu, const double *gpu)
{
    if (isnan(gpu[0]))
        return NULL;
    unsigned mismatches = 0u;
    for (size_t k = 0; k < count; k++) {
        mu_assert("CPU adm output is non-finite", isfinite(cpu[k]));
        if (cpu[k] == gpu[k])
            continue;
        mismatches++;
        (void)fprintf(stderr, "\n%s %ux%u %u-bit %s: cpu=%.17g cuda=%.17g delta=%.3e\n", what,
                      fx->w, fx->h, fx->bpc, keys[k], cpu[k], gpu[k], fabs(cpu[k] - gpu[k]));
    }
    mu_assert("adm_cuda differs from the CPU extractor", mismatches == 0u);
    return NULL;
}

static char *check_exact(const char *what, const Fixture *fx,
                         VmafFeatureDictionary *(*make_opts)(void), const char *const *keys,
                         size_t count)
{
    double cpu[MAX_KEYS];
    double gpu[MAX_KEYS];
    mu_assert("check_exact: too many keys", count <= MAX_KEYS);
    char *msg = run_adm(false, fx, make_opts ? make_opts() : NULL, keys, count, cpu);
    if (!msg)
        msg = run_adm(true, fx, make_opts ? make_opts() : NULL, keys, count, gpu);
    return msg ? msg : compare_runs(what, fx, keys, count, cpu, gpu);
}

static VmafFeatureDictionary *opts_from(const char *const pairs[][2], size_t count)
{
    VmafFeatureDictionary *d = NULL;
    for (size_t i = 0; i < count; i++) {
        if (vmaf_feature_dictionary_set(&d, pairs[i][0], pairs[i][1])) {
            (void)vmaf_feature_dictionary_free(&d);
            return NULL;
        }
    }
    return d;
}

/* The seven scores plus, with `debug=true`, the frame ratio and the sums it
 * is formed from. `suffix` is the feature-name suffix of the option set. */
#define ADM_SCORE_KEYS(suffix)                                                                     \
    "integer_adm2" suffix, "integer_aim" suffix, "integer_adm3" suffix,                            \
        "integer_adm_scale0" suffix, "integer_adm_scale1" suffix, "integer_adm_scale2" suffix,     \
        "integer_adm_scale3" suffix
#define ADM_DEBUG_KEYS(suffix)                                                                     \
    "integer_adm" suffix, "integer_adm_num" suffix, "integer_adm_den" suffix,                      \
        "integer_adm_num_scale0" suffix, "integer_adm_den_scale0" suffix,                          \
        "integer_adm_num_scale1" suffix, "integer_adm_den_scale1" suffix,                          \
        "integer_adm_num_scale2" suffix, "integer_adm_den_scale2" suffix,                          \
        "integer_adm_num_scale3" suffix, "integer_adm_den_scale3" suffix

static VmafFeatureDictionary *debug_opts(void)
{
    static const char *const pairs[][2] = {{"debug", "true"}};
    return opts_from(pairs, 1u);
}

static const char *const DEBUG_KEYS[] = {
    "VMAF_integer_feature_adm2_score",
    "VMAF_integer_feature_aim_score",
    "VMAF_integer_feature_adm3_score",
    "integer_adm_scale0",
    "integer_adm_scale1",
    "integer_adm_scale2",
    "integer_adm_scale3",
    ADM_DEBUG_KEYS(""),
};
#define NUM_DEBUG_KEYS (sizeof(DEBUG_KEYS) / sizeof(DEBUG_KEYS[0]))

/* The default model `vmaf_v1.0.16_3d0h` asks integer ADM for
 * VMAF_integer_feature_adm3_score with exactly these options. Because
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
    return opts_from(pairs, sizeof(pairs) / sizeof(pairs[0]));
}

#define MODEL_SUFFIX "_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02_apn_2"
static const char *const MODEL_KEYS[] = {ADM_SCORE_KEYS(MODEL_SUFFIX)};
#define NUM_MODEL_KEYS (sizeof(MODEL_KEYS) / sizeof(MODEL_KEYS[0]))

/* ADR-2795: one context at two viewing distances. The first distance keeps
 * its names and, with `debug`, the debug scores; the second files its seven
 * under the names `adm_norm_view_dist=5` gives them. */
static VmafFeatureDictionary *two_view_opts(void)
{
    static const char *const pairs[][2] = {{"debug", "true"}, {"adm_norm_view_dist_extra", "5"}};
    return opts_from(pairs, 2u);
}

static const char *const TWO_VIEW_KEYS[] = {
    "VMAF_integer_feature_adm2_score",
    "VMAF_integer_feature_aim_score",
    "VMAF_integer_feature_adm3_score",
    "integer_adm_scale0",
    "integer_adm_scale1",
    "integer_adm_scale2",
    "integer_adm_scale3",
    ADM_DEBUG_KEYS(""),
    ADM_SCORE_KEYS("_nvd_5"),
};
#define NUM_TWO_VIEW_KEYS (sizeof(TWO_VIEW_KEYS) / sizeof(TWO_VIEW_KEYS[0]))

/* The model options at 3H, then at 5H: the vmaf_v1.0.16_3d0h / _5d0h pair. */
static VmafFeatureDictionary *model_opts_at(const char *nvd_key, const char *nvd)
{
    VmafFeatureDictionary *d = model_opts();
    if (d && vmaf_feature_dictionary_set(&d, nvd_key, nvd)) {
        (void)vmaf_feature_dictionary_free(&d);
    }
    return d;
}

static VmafFeatureDictionary *two_view_model_opts(void)
{
    return model_opts_at("adm_norm_view_dist_extra", "5");
}

static const char *const TWO_VIEW_MODEL_KEYS[] = {
    ADM_SCORE_KEYS(MODEL_SUFFIX),
    ADM_SCORE_KEYS("_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02_nvd_5_apn_2"),
};
#define NUM_TWO_VIEW_MODEL_KEYS (sizeof(TWO_VIEW_MODEL_KEYS) / sizeof(TWO_VIEW_MODEL_KEYS[0]))

static VmafFeatureDictionary *barten_opts(void)
{
    static const char *const pairs[][2] = {{"adm_csf_mode", "1"}};
    return opts_from(pairs, 1u);
}

static const char *const BARTEN_KEYS[] = {ADM_SCORE_KEYS("_csf_1")};
#define NUM_BARTEN_KEYS (sizeof(BARTEN_KEYS) / sizeof(BARTEN_KEYS[0]))

/* adm_skip_scale0: the CPU leaves the scale-0 numerator at 0 and seeds its
 * denominator with 1e-10 narrowed to float, and both enter the frame sums. */
static VmafFeatureDictionary *skip_scale0_opts(void)
{
    static const char *const pairs[][2] = {{"adm_skip_scale0", "true"}, {"debug", "true"}};
    return opts_from(pairs, 2u);
}

static const char *const SKIP_SCALE0_KEYS[] = {ADM_SCORE_KEYS("_ssz"), ADM_DEBUG_KEYS("_ssz")};
#define NUM_SKIP_SCALE0_KEYS (sizeof(SKIP_SCALE0_KEYS) / sizeof(SKIP_SCALE0_KEYS[0]))

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
 * CUDA twin, not merely declared: the seven emitted values are the CPU's under
 * the default model's option dict. */
static char *test_adm_model_options_exact(void)
{
    const Fixture fx = {FIXTURE_W, FIXTURE_H, 8u, false, false};
    return check_exact("adm model-opt", &fx, model_opts, MODEL_KEYS, NUM_MODEL_KEYS);
}

static char *test_adm_barten_mode_exact(void)
{
    const Fixture fx = {FIXTURE_W, FIXTURE_H, 8u, false, false};
    return check_exact("adm Barten mode", &fx, barten_opts, BARTEN_KEYS, NUM_BARTEN_KEYS);
}

static char *test_adm_cuda_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm_cuda");
    mu_assert("adm_cuda extractor must be registered", fex != NULL);
    mu_assert("adm_cuda name matches", !strcmp(fex->name, "adm_cuda"));
    return NULL;
}

/* Default options, every output including the per-scale sums. */
static char *test_adm_default_exact(void)
{
    const Fixture fx = {FIXTURE_W, FIXTURE_H, 8u, false, false};
    return check_exact("adm", &fx, debug_opts, DEBUG_KEYS, NUM_DEBUG_KEYS);
}

static char *test_adm_10bit_exact(void)
{
    const Fixture fx = {FIXTURE_W, FIXTURE_H, 10u, false, false};
    return check_exact("adm", &fx, debug_opts, DEBUG_KEYS, NUM_DEBUG_KEYS);
}

/* Odd at every scale: 322x182 halves to 161x91, 81x46, 41x23 and 21x12. */
static char *test_adm_odd_frame_exact(void)
{
    const Fixture fx = {322u, 182u, 8u, false, false};
    return check_exact("adm", &fx, debug_opts, DEBUG_KEYS, NUM_DEBUG_KEYS);
}

/* 3840x2160: the scale-0 denominator region exceeds 2^20 samples, so its row
 * sums are shifted before they are added (shift_accum = 1), and a scale-0 row
 * is wider than one block of the old kernel. */
static char *test_adm_2160p_exact(void)
{
    const Fixture fx = {3840u, 2160u, 8u, false, false};
    return check_exact("adm", &fx, debug_opts, DEBUG_KEYS, NUM_DEBUG_KEYS);
}

/* Almost no reference detail: the denominator accumulators of the coarse
 * scales are small enough that folding each warp of a row on its own, instead
 * of the row, moves `integer_adm_den_scale2` and `_scale3` (ADR-1416). */
static char *test_adm_sparse_detail_exact(void)
{
    const Fixture fx = {640u, 360u, 8u, true, false};
    return check_exact("adm sparse", &fx, debug_opts, DEBUG_KEYS, NUM_DEBUG_KEYS);
}

/* 962x13542: the scale-0 denominator region is 387 x 5419 = 2^21 + 1 samples.
 * The CPU's shift is ceil(log2(area) - 20) = 2 in double; the fp32 device
 * logarithm the kernel used rounds log2(2^21 + 1) to exactly 21 and shifted by
 * 1, while the host concluded with 2, which put `integer_adm_scale0` at 0.86
 * instead of 0.98. 81 region areas up to 2^26 behave like this one. The
 * kernels now take every shift from the CPU's context (ADR-1416). */
static char *test_adm_shift_boundary_area_exact(void)
{
    const Fixture fx = {962u, 13542u, 8u, false, false};
    return check_exact("adm shift boundary", &fx, debug_opts, DEBUG_KEYS, NUM_DEBUG_KEYS);
}

/* Isolated large-amplitude patches, the distorted frame at 60-99% of them
 * (T-GPU-ADM-DECOUPLE-FP32-RECIPROCAL-2026-10-03): the scale-0 decouple takes
 * the reciprocal 2^30 / o from the CPU's integer quotient. An fp32 quotient
 * truncates to another integer for 343 positive operands, which moves the
 * ratio k and, for a reference coefficient above 16566, the restored sample. */
static char *test_adm_attenuated_detail_exact(void)
{
    const Fixture fx = {224u, 224u, 8u, false, true};
    mu_assert_msg(
        check_exact("adm attenuated detail", &fx, debug_opts, DEBUG_KEYS, NUM_DEBUG_KEYS));
    return check_exact("adm attenuated detail", &fx, model_opts, MODEL_KEYS, NUM_MODEL_KEYS);
}

static char *test_adm_skip_scale0_exact(void)
{
    const Fixture fx = {FIXTURE_W, FIXTURE_H, 8u, false, false};
    return check_exact("adm skip scale 0", &fx, skip_scale0_opts, SKIP_SCALE0_KEYS,
                       NUM_SKIP_SCALE0_KEYS);
}

/* ADR-2795: a second viewing distance on the CUDA twin returns the CPU's
 * scores for both distances, at 8 and 10 bits. */
static char *test_adm_two_views_exact(void)
{
    const Fixture fx8 = {FIXTURE_W, FIXTURE_H, 8u, false, false};
    const Fixture fx10 = {FIXTURE_W, FIXTURE_H, 10u, false, false};
    mu_assert_msg(
        check_exact("adm two views", &fx8, two_view_opts, TWO_VIEW_KEYS, NUM_TWO_VIEW_KEYS));
    mu_assert_msg(check_exact("adm two views 10-bit", &fx10, two_view_opts, TWO_VIEW_KEYS,
                              NUM_TWO_VIEW_KEYS));
    return check_exact("adm two views model options", &fx8, two_view_model_opts,
                       TWO_VIEW_MODEL_KEYS, NUM_TWO_VIEW_MODEL_KEYS);
}

/* ADR-2795: the CUDA twin registered at 3H and at 5H (the registry folds the
 * second into the first, as two models do) scores as one CPU context with
 * both distances. */
static char *test_adm_merged_registrations_exact(void)
{
    const Fixture fx = {FIXTURE_W, FIXTURE_H, 8u, false, false};
    double cpu[MAX_KEYS];
    double gpu[MAX_KEYS];
    char *msg = run_adm(false, &fx, two_view_model_opts(), TWO_VIEW_MODEL_KEYS,
                        NUM_TWO_VIEW_MODEL_KEYS, cpu);
    if (!msg) {
        msg = run_adm2(true, &fx, model_opts(), model_opts_at("adm_norm_view_dist", "5"),
                       TWO_VIEW_MODEL_KEYS, NUM_TWO_VIEW_MODEL_KEYS, gpu);
    }
    return msg ? msg :
                 compare_runs("adm merged registrations", &fx, TWO_VIEW_MODEL_KEYS,
                              NUM_TWO_VIEW_MODEL_KEYS, cpu, gpu);
}

/* The exact cases with default options, one per cause of ADR-1416. */
static char *run_exact_default_cases(void)
{
    mu_run_test(test_adm_default_exact);
    mu_run_test(test_adm_10bit_exact);
    mu_run_test(test_adm_odd_frame_exact);
    mu_run_test(test_adm_2160p_exact);
    mu_run_test(test_adm_sparse_detail_exact);
    mu_run_test(test_adm_shift_boundary_area_exact);
    mu_run_test(test_adm_attenuated_detail_exact);
    return NULL;
}

static char *run_exact_option_cases(void)
{
    mu_run_test(test_adm_model_options_exact);
    mu_run_test(test_adm_barten_mode_exact);
    mu_run_test(test_adm_skip_scale0_exact);
    mu_run_test(test_adm_two_views_exact);
    mu_run_test(test_adm_merged_registrations_exact);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_adm_cuda_registered);
    mu_run_test(test_adm_cuda_option_table_mirrors_cpu);
    mu_assert_msg(run_exact_default_cases());
    mu_assert_msg(run_exact_option_cases());
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
