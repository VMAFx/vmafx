/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * adm CPU vs. HIP: every output has the CPU's bits (ADR-1423).
 *
 * Integer ADM is integer arithmetic up to its last step, so the twin can have
 * the CPU's accumulators. `adm_hip` takes its CSF weights, its border, its
 * rounding shifts and its float conclusion from the CPU's own routines
 * (integer_adm_kernels.h) and rounds the denominator once per row as the CPU
 * does. This test asserts equality, not a tolerance, on every output of every
 * case, the per-scale numerators and denominators of `debug=true` included,
 * and since ADR-1525 the AIM measure's aim and adm3.
 *
 * Before ADR-1423 the denominator kernels rounded every thread's partial sum
 * of a row on its own and derived their shifts from an fp32 logarithm, while
 * the host concluded with shifts of its own. The sparse, the 3840x2160 and
 * the shift-boundary case below fail on that twin.
 *
 * Skip behaviour: without a HIP device, or on a build without the device
 * kernels (-ENOSYS), a case reports the skip and passes.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

/* Fixture geometry — large enough for the 4-scale ADM DWT pyramid. */
#define FIXTURE_W 256u
#define FIXTURE_H 144u

#define MAX_KEYS 32u
/* Frames per run: the second one shows that a frame starts from cleared
 * accumulators. */
#define NUM_FRAMES 2u

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
    unsigned x = (row * 73856093u) ^ (col * 19349663u) ^ (salt * 83492791u);
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return x;
}

/* Luma of the textured fixture: a wrapping ramp with texture for the
 * reference; the distorted frame adds a position-dependent error, so every
 * scale has detail to lose and to mask. */
static unsigned textured_luma(unsigned row, unsigned col, unsigned frame, bool distorted)
{
    row += frame * 5u;
    unsigned v = (((row * 3u) + (col * 2u)) & 0xFFu) ^ (((row >> 2) * (col >> 3)) & 0x1Fu);
    if (distorted) {
        v += 9u + (((row * 5u) + (col * 7u)) % 11u);
    }
    return v;
}

/* Luma of the sparse fixture: mid grey with one sample in 200 raised by a
 * level, and a distorted frame that raises half of the samples by one more.
 * The reference bands are then almost empty, the denominator accumulators
 * are small integers, and where their rows are rounded shows in the score. */
static unsigned sparse_luma(unsigned row, unsigned col, unsigned frame, bool distorted)
{
    unsigned v = 128u + (((position_hash(row, col, 1u + (frame * 2u)) % 200u) == 0u) ? 1u : 0u);
    if (distorted) {
        v += position_hash(row, col, 2u + (frame * 2u)) & 1u;
    }
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
static unsigned graded_luma(unsigned row, unsigned col, unsigned frame, bool distorted)
{
    (void)frame; /* the same patches in every frame */
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
    uint8_t *line = (uint8_t *)pic->data[plane] + ((size_t)row * (size_t)pic->stride[plane]);
    if (pic->bpc <= 8u) {
        line[col] = (uint8_t)(v > peak ? peak : v);
    } else {
        ((uint16_t *)line)[col] = (uint16_t)(v > peak ? peak : v);
    }
}

/* Luma of one sample of the fixture `fx`. */
static unsigned fixture_luma(const Fixture *fx, unsigned row, unsigned col, unsigned frame,
                             bool distorted)
{
    if (fx->graded) {
        return graded_luma(row, col, frame, distorted);
    }
    if (fx->sparse) {
        return sparse_luma(row, col, frame, distorted);
    }
    return textured_luma(row, col, frame, distorted);
}

static int fill_picture(VmafPicture *pic, const Fixture *fx, unsigned frame, bool distorted)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, fx->bpc, fx->w, fx->h);
    if (err) {
        return err;
    }
    const unsigned gain = 1u << (fx->bpc - 8u);
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const unsigned v = fixture_luma(fx, row, col, frame, distorted);
            put_sample(pic, 0u, row, col, v * gain);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                put_sample(pic, p, row, col, 128u * gain);
            }
        }
    }
    return 0;
}

/* Frame `frame` of the fixture through `vmaf`, which takes both pictures. */
static int feed_frame(VmafContext *vmaf, const Fixture *fx, unsigned frame)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_picture(&ref, fx, frame, false);
    if (err) {
        return err;
    }
    err = fill_picture(&dist, fx, frame, true);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, frame);
}

/* A context with the CPU `adm` extractor, or with `adm_hip` on `hip_state`,
 * registered with `opts` and, when `opts2` is set, a second time with it (the
 * registry folds the second into the first, ADR-2795). Both are consumed. */
static int adm_context(VmafContext **vmaf, VmafHipState *hip_state, VmafFeatureDictionary *opts,
                       VmafFeatureDictionary *opts2)
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    const char *name = hip_state ? "adm_hip" : "adm";
    int err = vmaf_init(vmaf, cfg);
    if (!err && hip_state) {
        err = vmaf_hip_import_state(*vmaf, hip_state);
    }
    if (!err) {
        err = vmaf_use_feature(*vmaf, name, opts);
        opts = err ? opts : NULL; /* taken on success */
    }
    if (!err && opts2) {
        err = vmaf_use_feature(*vmaf, name, opts2);
        opts2 = err ? opts2 : NULL;
    }
    if (opts) {
        (void)vmaf_feature_dictionary_free(&opts);
    }
    if (opts2) {
        (void)vmaf_feature_dictionary_free(&opts2);
    }
    return err;
}

/* NUM_FRAMES frames through one extractor, and every key in `keys` of every
 * frame read into `out` (frame-major). Returns the first error; -ENOSYS is
 * the scaffold build. */
static int adm_scores(VmafHipState *hip_state, const Fixture *fx, VmafFeatureDictionary *opts,
                      VmafFeatureDictionary *opts2, const char *const *keys, size_t count,
                      double *out)
{
    VmafContext *vmaf = NULL;
    int err = adm_context(&vmaf, hip_state, opts, opts2);
    for (unsigned frame = 0; frame < NUM_FRAMES && !err; frame++) {
        err = feed_frame(vmaf, fx, frame);
    }
    if (!err) {
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    }
    for (size_t i = 0; i < count * NUM_FRAMES && !err; i++) {
        err = vmaf_feature_score_at_index(vmaf, keys[i % count], &out[i], (unsigned)(i / count));
        if (err) {
            (void)fprintf(stderr, "\nmissing feature-name key: %s (%s)\n", keys[i % count],
                          hip_state ? "HIP" : "CPU");
        }
    }
    const int closed = vmaf ? vmaf_close(vmaf) : 0;
    return err ? err : closed;
}

/* The device, or NULL with the reason printed when there is none. */
static VmafHipState *hip_device(void)
{
    VmafHipState *hip_state = NULL;
    const VmafHipConfiguration hip_cfg = {.device_index = -1};
    if (vmaf_hip_state_init(&hip_state, hip_cfg) != 0 || hip_state == NULL) {
        (void)fprintf(stderr, "[skip: no HIP device] ");
        mu_skipped = 1;
        return NULL;
    }
    return hip_state;
}

/* The HIP leg's options: the case's own, or, for the merged registrations
 * (ADR-2795), two registrations against the CPU's one. */
typedef struct AdmCaseOpts {
    VmafFeatureDictionary *(*cpu)(void);
    VmafFeatureDictionary *(*hip)(void);
    VmafFeatureDictionary *(*hip2)(void); /* NULL: one registration */
} AdmCaseOpts;

/* The keys of the case whose HIP value is not the CPU's, each one reported. */
static unsigned count_mismatches(const char *what, const Fixture *fx, const char *const *keys,
                                 size_t count, const double *cpu, const double *gpu)
{
    unsigned mismatches = 0u;
    for (size_t i = 0; i < count * NUM_FRAMES; i++) {
        if (isfinite(cpu[i]) && cpu[i] == gpu[i]) {
            continue;
        }
        mismatches++;
        (void)fprintf(stderr, "\n%s %ux%u %u-bit frame %u %s: cpu=%.17g hip=%.17g delta=%.3e\n",
                      what, fx->w, fx->h, fx->bpc, (unsigned)(i / count), keys[i % count], cpu[i],
                      gpu[i], fabs(cpu[i] - gpu[i]));
    }
    return mismatches;
}

/* The mismatching keys of a case with the HIP and CPU options of `o`;
 * UINT32_MAX when a run failed. A skipped HIP leg counts as 0. */
static unsigned case_mismatches(const char *what, const Fixture *fx, const AdmCaseOpts *o,
                                const char *const *keys, size_t count)
{
    double cpu[MAX_KEYS * NUM_FRAMES] = {0.0};
    double gpu[MAX_KEYS * NUM_FRAMES] = {0.0};
    if (count > MAX_KEYS) {
        return UINT32_MAX;
    }
    VmafHipState *hip_state = hip_device();
    if (!hip_state) {
        return 0u;
    }
    const int gpu_err =
        adm_scores(hip_state, fx, o->hip(), o->hip2 ? o->hip2() : NULL, keys, count, gpu);
    vmaf_hip_state_free(&hip_state);
    if (gpu_err == -ENOSYS) {
        (void)fprintf(stderr, "[skip: HIP kernels not built (enable_hipcc=false)] ");
        mu_skipped = 1;
        return 0u;
    }
    const int cpu_err = gpu_err ? 0 : adm_scores(NULL, fx, o->cpu(), NULL, keys, count, cpu);
    if (gpu_err || cpu_err) {
        (void)fprintf(stderr, "\n%s %ux%u: run failed (hip %d, cpu %d)\n", what, fx->w, fx->h,
                      gpu_err, cpu_err);
        return UINT32_MAX;
    }
    return count_mismatches(what, fx, keys, count, cpu, gpu);
}

/* The keys of the case whose HIP value is not the CPU's with `make_opts()`
 * on both twins. */
static unsigned exact_mismatches(const char *what, const Fixture *fx,
                                 VmafFeatureDictionary *(*make_opts)(void), const char *const *keys,
                                 size_t count)
{
    const AdmCaseOpts o = {.cpu = make_opts, .hip = make_opts, .hip2 = NULL};
    return case_mismatches(what, fx, &o, keys, count);
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

/* The seven scores `adm_hip` provides, aim and adm3 from its AIM pass
 * (ADR-1525) included, plus, with `debug=true`, the frame ratio and the sums
 * it is formed from. `suffix` is the feature-name suffix of the option set. */
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

/* The options the default model `vmaf_v1.0.16_3d0h` passes to integer ADM.
 * Every one is a VMAF_OPT_FLAG_FEATURE_PARAM and non-default, so both twins
 * emit the fully-suffixed keys. */
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

/* adm_skip_aim: no AIM numerator, so aim = 0 and adm3 blends the DLM ratio
 * with 0. Not a feature parameter: the keys keep their names. */
static VmafFeatureDictionary *skip_aim_opts(void)
{
    static const char *const pairs[][2] = {{"adm_skip_aim", "true"}, {"debug", "true"}};
    return opts_from(pairs, 2u);
}

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

static VmafFeatureDictionary *model_opts_5h(void)
{
    return model_opts_at("adm_norm_view_dist", "5");
}

static const char *const TWO_VIEW_MODEL_KEYS[] = {
    ADM_SCORE_KEYS(MODEL_SUFFIX),
    ADM_SCORE_KEYS("_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02_nvd_5_apn_2"),
};
#define NUM_TWO_VIEW_MODEL_KEYS (sizeof(TWO_VIEW_MODEL_KEYS) / sizeof(TWO_VIEW_MODEL_KEYS[0]))

/* Default options, every output including the per-scale sums. */
static char *test_adm_default_exact(void)
{
    const Fixture fx = {FIXTURE_W, FIXTURE_H, 8u, false, false};
    mu_assert("adm_hip differs from the CPU extractor",
              exact_mismatches("adm", &fx, debug_opts, DEBUG_KEYS, NUM_DEBUG_KEYS) == 0u);
    return NULL;
}

static char *test_adm_10bit_exact(void)
{
    const Fixture fx = {FIXTURE_W, FIXTURE_H, 10u, false, false};
    mu_assert("adm_hip differs from the CPU extractor at 10 bits",
              exact_mismatches("adm", &fx, debug_opts, DEBUG_KEYS, NUM_DEBUG_KEYS) == 0u);
    return NULL;
}

/* Odd at every scale: 322x182 halves to 161x91, 81x46, 41x23 and 21x12. */
static char *test_adm_odd_frame_exact(void)
{
    const Fixture fx = {322u, 182u, 8u, false, false};
    mu_assert("adm_hip differs from the CPU extractor on an odd frame",
              exact_mismatches("adm", &fx, debug_opts, DEBUG_KEYS, NUM_DEBUG_KEYS) == 0u);
    return NULL;
}

/* 3840x2160: the scale-0 denominator region exceeds 2^20 samples, so its row
 * sums are shifted before they are added (shift_accum = 1), which is where a
 * fold per thread and a fold per row part. */
static char *test_adm_2160p_exact(void)
{
    const Fixture fx = {3840u, 2160u, 8u, false, false};
    mu_assert("adm_hip differs from the CPU extractor at 3840x2160",
              exact_mismatches("adm", &fx, debug_opts, DEBUG_KEYS, NUM_DEBUG_KEYS) == 0u);
    return NULL;
}

/* Almost no reference detail: the denominator accumulators of the coarse
 * scales are small enough that folding each thread's share of a row on its
 * own, instead of the row, moves `integer_adm_den_scale2` and `_scale3`. */
static char *test_adm_sparse_detail_exact(void)
{
    const Fixture fx = {640u, 360u, 8u, true, false};
    mu_assert("adm_hip differs from the CPU extractor on a low-detail frame",
              exact_mismatches("adm sparse", &fx, debug_opts, DEBUG_KEYS, NUM_DEBUG_KEYS) == 0u);
    return NULL;
}

/* 962x13542: the scale-0 denominator region is 387 x 5419 = 2^21 + 1 samples.
 * The CPU's shift is ceil(log2(area) - 20) = 2 in double; an fp32 logarithm
 * rounds log2(2^21 + 1) to exactly 21 and gives 1. A kernel that shifts by
 * one while the host concludes with two doubles the denominator. 81 region
 * areas up to 2^26 behave like this one. The kernels take every shift from
 * the CPU's context. */
static char *test_adm_shift_boundary_area_exact(void)
{
    const Fixture fx = {962u, 13542u, 8u, false, false};
    mu_assert("adm_hip differs from the CPU extractor where the fp32 shift is off by one",
              exact_mismatches("adm shift boundary", &fx, debug_opts, DEBUG_KEYS, NUM_DEBUG_KEYS) ==
                  0u);
    return NULL;
}

/* csf_mode / p_norm / dlm_weight / min_val / noise_weight under the default
 * model's option dict. */
static char *test_adm_model_options_exact(void)
{
    const Fixture fx = {FIXTURE_W, FIXTURE_H, 8u, false, false};
    mu_assert("adm_hip differs from the CPU extractor with the default model's options",
              exact_mismatches("adm model-opt", &fx, model_opts, MODEL_KEYS, NUM_MODEL_KEYS) == 0u);
    return NULL;
}

static char *test_adm_barten_mode_exact(void)
{
    const Fixture fx = {FIXTURE_W, FIXTURE_H, 8u, false, false};
    mu_assert("adm_hip differs from the CPU extractor in Barten mode",
              exact_mismatches("adm Barten", &fx, barten_opts, BARTEN_KEYS, NUM_BARTEN_KEYS) == 0u);
    return NULL;
}

static char *test_adm_skip_aim_exact(void)
{
    const Fixture fx = {FIXTURE_W, FIXTURE_H, 8u, false, false};
    mu_assert("adm_hip differs from the CPU extractor with adm_skip_aim",
              exact_mismatches("adm skip aim", &fx, skip_aim_opts, DEBUG_KEYS, NUM_DEBUG_KEYS) ==
                  0u);
    return NULL;
}

/* Isolated large-amplitude patches, the distorted frame at 60-99% of them
 * (T-GPU-ADM-DECOUPLE-FP32-RECIPROCAL-2026-10-03): the scale-0 decouple takes
 * the reciprocal 2^30 / o from the CPU's integer quotient. An fp32 quotient
 * truncates to another integer for 343 positive operands, which moves the
 * ratio k and, for a reference coefficient above 16566, the restored sample. */
static char *test_adm_attenuated_detail_exact(void)
{
    const Fixture fx = {224u, 224u, 8u, false, true};
    mu_assert("adm_hip differs from the CPU extractor on attenuated detail",
              exact_mismatches("adm attenuated", &fx, debug_opts, DEBUG_KEYS, NUM_DEBUG_KEYS) ==
                  0u);
    mu_assert("adm_hip differs from the CPU extractor on attenuated detail, model options",
              exact_mismatches("adm attenuated model-opt", &fx, model_opts, MODEL_KEYS,
                               NUM_MODEL_KEYS) == 0u);
    return NULL;
}

static char *test_adm_skip_scale0_exact(void)
{
    const Fixture fx = {FIXTURE_W, FIXTURE_H, 8u, false, false};
    mu_assert("adm_hip differs from the CPU extractor with adm_skip_scale0",
              exact_mismatches("adm skip scale 0", &fx, skip_scale0_opts, SKIP_SCALE0_KEYS,
                               NUM_SKIP_SCALE0_KEYS) == 0u);
    return NULL;
}

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

/* ADR-2795: a second viewing distance on the HIP twin returns the CPU's
 * scores for both distances, at 8 and 10 bits and under the model options. */
static char *test_adm_two_views_exact(void)
{
    const Fixture fx8 = {FIXTURE_W, FIXTURE_H, 8u, false, false};
    const Fixture fx10 = {FIXTURE_W, FIXTURE_H, 10u, false, false};
    mu_assert("adm_hip differs from the CPU at two viewing distances",
              exact_mismatches("adm two views", &fx8, two_view_opts, TWO_VIEW_KEYS,
                               NUM_TWO_VIEW_KEYS) == 0u);
    mu_assert("adm_hip differs from the CPU at two viewing distances, 10-bit",
              exact_mismatches("adm two views 10-bit", &fx10, two_view_opts, TWO_VIEW_KEYS,
                               NUM_TWO_VIEW_KEYS) == 0u);
    mu_assert("adm_hip differs from the CPU at two viewing distances with the model options",
              exact_mismatches("adm two views model options", &fx8, two_view_model_opts,
                               TWO_VIEW_MODEL_KEYS, NUM_TWO_VIEW_MODEL_KEYS) == 0u);
    return NULL;
}

/* ADR-2795: the HIP twin registered at 3H and at 5H (the registry folds the
 * second into the first, as two models do) scores as one CPU context with
 * both distances. */
static char *test_adm_merged_registrations_exact(void)
{
    const Fixture fx = {FIXTURE_W, FIXTURE_H, 8u, false, false};
    const AdmCaseOpts o = {.cpu = two_view_model_opts, .hip = model_opts, .hip2 = model_opts_5h};
    mu_assert("adm_hip registered at 3H and 5H differs from one CPU context at both",
              case_mismatches("adm merged registrations", &fx, &o, TWO_VIEW_MODEL_KEYS,
                              NUM_TWO_VIEW_MODEL_KEYS) == 0u);
    return NULL;
}

static char *run_exact_option_cases(void)
{
    mu_run_test(test_adm_model_options_exact);
    mu_run_test(test_adm_barten_mode_exact);
    mu_run_test(test_adm_skip_scale0_exact);
    mu_run_test(test_adm_skip_aim_exact);
    mu_run_test(test_adm_two_views_exact);
    mu_run_test(test_adm_merged_registrations_exact);
    return NULL;
}

char *run_tests(void)
{
    mu_assert_msg(run_exact_default_cases());
    mu_assert_msg(run_exact_option_cases());
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
