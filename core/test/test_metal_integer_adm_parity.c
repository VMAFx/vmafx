/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * adm CPU vs. Metal: every output has the CPU's bits (ADR-1416 for the CUDA
 * twin, ADR-1413 and ADR-1402 for the CPU arithmetic every twin reproduces).
 *
 * The integer ADM extractor is registered as `adm` on the CPU
 * (core/src/feature/integer_adm.c); the Metal twin is `integer_adm_metal`.
 * Both emit the aggregate `VMAF_integer_feature_adm2_score`, the AIM and ADM3
 * scores and the four `integer_adm_scale[0..3]` sub-scores, and with
 * `debug=true` the frame ratio and the per-scale numerators and denominators.
 * Every case asserts equality, not a tolerance, on every key the CPU emits.
 *
 * The cases follow test_cuda_adm_parity.c (default, 10 bits, odd frame, 2160p,
 * sparse detail, the shift boundary, the default model's options, Barten
 * mode, adm_skip_scale0) and test_gpu_adm_tiny_frames.c (tiny geometries 17 to
 * 32 pixels, full-range noise, bright 16-bit samples, isolated patches, the
 * rejected sizes, and the enhanced-contrast picture at fractional gain limits
 * against the SCALAR CPU path: the cpumask of the CPU leg disables every SIMD
 * routine).
 *
 * State row measured: T-METAL-ADM-GAIN-LIMIT-FLOAT32-2026-10-01. The Metal
 * twin multiplies the restored sample by adm_enhn_gain_limit in binary32; the
 * CPU truncates the double product rst * adm_enhn_gain_limit toward zero
 * (ADR-1413). The row closes when the twin matches the scalar CPU at limits
 * 1.2 and 1.5: test_adm_gain_limit_1_2_exact and test_adm_gain_limit_1_5_exact
 * score a picture whose contrast the distorted copy doubles, so the limit
 * binds, and a precondition of each checks that the scalar CPU's scores move
 * with the limit.
 *
 * Skip behaviour: without a Metal device every case reports the skip and the
 * run exits 77. The registration case needs no device. The size-rejection case
 * sends frames below 17x17 to the twin through a context; the CPU extractor
 * rejects them with -EINVAL too, so the self-test runs it as well.
 */

#include "float_bits.h"
#include "metal_twin.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "feature/feature_extractor.h"
#include "libvmaf/feature.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static const char *const TWIN_NAME = METAL_TWIN("integer_adm_metal", "adm");

#define MAX_KEYS 32u
#define MAX_OPTS 8u

typedef struct Geometry {
    unsigned w;
    unsigned h;
} Geometry;

/* Luma sample of the reference (distorted == false) or the distorted picture,
 * in the sample range of `bpc`. */
typedef unsigned (*SampleFn)(unsigned row, unsigned col, bool distorted, unsigned bpc);

/* One comparison of the twin with the CPU. */
typedef struct AdmCase {
    const char *name;
    Geometry geometry;
    unsigned bpc;
    SampleFn sample;
    bool scalar; /* the CPU leg runs with every SIMD routine disabled */
    size_t n_opts;
    const char *opts[MAX_OPTS][2];
    size_t n_keys;
    const char *keys[MAX_KEYS];
    /* ADR-2795: register the twin a second time at this adm_norm_view_dist,
     * as two models do, against one CPU context with it as
     * adm_norm_view_dist_extra; NULL for one registration. */
    const char *merge_nvd;
} AdmCase;

/* A 32-bit integer hash (lowbias32) of the position, seeded per picture.
 * Stateless, so both sides see the same frames. */
static uint32_t position_hash(unsigned row, unsigned col, unsigned salt)
{
    uint32_t x = ((uint32_t)row << 16) ^ (uint32_t)col ^ (salt * 0x9E3779B9u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* A wrapping ramp with texture for the reference; the distorted frame adds a
 * position-dependent error, so every scale has detail to lose and to mask. */
static unsigned textured_sample(unsigned row, unsigned col, bool distorted, unsigned bpc)
{
    unsigned v = ((row * 3u + col * 2u) & 0xFFu) ^ (((row >> 2) * (col >> 3)) & 0x1Fu);
    if (distorted) {
        v += 9u + ((row * 5u + col * 7u) % 11u);
    }
    return v << (bpc - 8u);
}

/* Mid grey with one sample in 200 raised by a level, and a distorted frame
 * that raises half of the samples by one more. The reference bands are then
 * almost empty, the denominator accumulators are small integers, and where
 * their rows are rounded shows in the score. */
static unsigned sparse_sample(unsigned row, unsigned col, bool distorted, unsigned bpc)
{
    unsigned v = 128u + (position_hash(row, col, 1u) % 200u == 0u ? 1u : 0u);
    if (distorted) {
        v += position_hash(row, col, 2u) & 1u;
    }
    return v << (bpc - 8u);
}

/* The same ramp and periodic error as test_integer_adm_tiny_frames.c. */
static unsigned ramp_sample(unsigned row, unsigned col, bool distorted, unsigned bpc)
{
    (void)bpc;
    unsigned v = (row * 5u + col * 3u) & 0xFFu;
    if (distorted) {
        v = (v + ((row * 7u + col) % 17u)) & 0xFFu;
    }
    return v;
}

/* Full-range 8-bit noise, independent between the two pictures. */
static unsigned noise_sample(unsigned row, unsigned col, bool distorted, unsigned bpc)
{
    (void)bpc;
    return position_hash(row, col, distorted ? 1u : 0u) >> 24;
}

/* Bright 16-bit noise in [49152, 65535]: every scale-0 column overflows an
 * int32 vertical sum (three samples of 42456 or more). */
static unsigned bright16_sample(unsigned row, unsigned col, bool distorted, unsigned bpc)
{
    (void)bpc;
    return 49152u + (position_hash(row, col, distorted ? 1u : 0u) >> 18);
}

/* Flat mid grey; the distorted picture adds a white column next to three
 * black ones, two rows high, every 16 pixels. Each patch gives one large
 * scale-0 coefficient with small neighbours. */
static unsigned patch_sample(unsigned row, unsigned col, bool distorted, unsigned bpc)
{
    (void)bpc;
    const unsigned patch_row = (row + 13u) % 16u;
    const unsigned patch_col = (col + 13u) % 16u;
    if (!distorted || patch_row > 1u || patch_col > 3u) {
        return 128u;
    }
    return (patch_col == 0u) ? 255u : 0u;
}

/* Low-contrast noise around mid grey; the distorted picture doubles the
 * contrast. Doubling keeps the direction of every (h, v) coefficient pair, so
 * the one-degree angle test passes, and takes the distorted coefficient past
 * any gain limit below 2: the decouple stores the limited product. */
static unsigned enhanced_sample(unsigned row, unsigned col, bool distorted, unsigned bpc)
{
    (void)bpc;
    const int noise = (int)(position_hash(row, col, 0u) >> 26) - 32;
    return (unsigned)(128 + (distorted ? 2 * noise : noise));
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

static int fill_picture(VmafPicture *pic, const AdmCase *c, bool distorted)
{
    const int err =
        vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, c->bpc, c->geometry.w, c->geometry.h);
    if (err) {
        return err;
    }
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            put_sample(pic, 0u, row, col, c->sample(row, col, distorted, c->bpc));
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                put_sample(pic, p, row, col, 1u << (c->bpc - 1u));
            }
        }
    }
    return 0;
}

/* The status of the first frame of the case through `vmaf`. */
static int feed_frame(VmafContext *vmaf, const AdmCase *c)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_picture(&ref, c, false);
    if (err) {
        return err;
    }
    err = fill_picture(&dist, c, true);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, 0u);
}

/* Register `name` with the case's options plus `extra` = `value` when
 * `extra` is set. */
static int use_adm(VmafContext *vmaf, const char *name, const AdmCase *c, const char *extra,
                   const char *value)
{
    VmafFeatureDictionary *opts = NULL;
    int err = 0;
    for (size_t i = 0; i < c->n_opts && !err; i++) {
        err = vmaf_feature_dictionary_set(&opts, c->opts[i][0], c->opts[i][1]);
    }
    if (!err && extra) {
        err = vmaf_feature_dictionary_set(&opts, extra, value);
    }
    if (!err) {
        /* vmaf_use_feature() takes the dictionary over, on failure too. */
        return vmaf_use_feature(vmaf, name, opts);
    }
    (void)vmaf_feature_dictionary_free(&opts);
    return err;
}

/* A context with the CPU `adm`, or with the twin on `state`, carrying the
 * case's options (and, with `merge_nvd`, the second viewing distance). */
static int adm_context(VmafContext **vmaf, void *state, const AdmCase *c)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    if (!state && c->scalar) {
        cfg.cpumask = ~(uint64_t)0;
    }
    int err = vmaf_init(vmaf, cfg);
    if (!err && state) {
        err = metal_twin_import(*vmaf, state);
    }
    if (err) {
        return err;
    }
    if (!state) {
        return use_adm(*vmaf, "adm", c, c->merge_nvd ? "adm_norm_view_dist_extra" : NULL,
                       c->merge_nvd);
    }
    err = use_adm(*vmaf, TWIN_NAME, c, NULL, NULL);
    if (!err && c->merge_nvd) {
        err = use_adm(*vmaf, TWIN_NAME, c, "adm_norm_view_dist", c->merge_nvd);
    }
    return err;
}

/* One frame through one extractor, and every key of the case read into `out`.
 * Returns the first error. */
static int adm_scores(void *state, const AdmCase *c, double *out)
{
    VmafContext *vmaf = NULL;
    int err = adm_context(&vmaf, state, c);
    if (!err) {
        err = feed_frame(vmaf, c);
    }
    if (!err) {
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    }
    for (size_t k = 0; k < c->n_keys && !err; k++) {
        err = vmaf_feature_score_at_index(vmaf, c->keys[k], &out[k], 0u);
        if (err) {
            (void)fprintf(stderr, "\n%s: no score for %s (%s)\n", c->name, c->keys[k],
                          state ? METAL_TWIN_BACKEND : "CPU");
        }
    }
    const int closed = vmaf ? vmaf_close(vmaf) : 0;
    return err ? err : closed;
}

/* The Metal state, or NULL with the reason printed when there is none. */
static void *metal_device(void)
{
    void *state = NULL;
    if (metal_twin_open(&state) != 0 || state == NULL) {
        (void)fprintf(stderr, "[skip: no Metal device] ");
        mu_skipped = 1;
        return NULL;
    }
    return state;
}

/* The outputs of the case whose twin value is not the CPU's, each one
 * reported; UINT32_MAX when a run failed. A skipped twin leg counts as 0. */
static unsigned exact_mismatches(const AdmCase *c)
{
    double cpu[MAX_KEYS] = {0.0};
    double gpu[MAX_KEYS] = {0.0};
    void *state = metal_device();
    if (!state) {
        return 0u;
    }
    const int gpu_err = adm_scores(state, c, gpu);
    (void)metal_twin_close(state);
    const int cpu_err = gpu_err ? 0 : adm_scores(NULL, c, cpu);
    if (gpu_err || cpu_err) {
        (void)fprintf(stderr, "\n%s %ux%u: run failed (%s %d, cpu %d)\n", c->name, c->geometry.w,
                      c->geometry.h, METAL_TWIN_BACKEND, gpu_err, cpu_err);
        return UINT32_MAX;
    }
    unsigned mismatches = 0u;
    for (size_t k = 0; k < c->n_keys; k++) {
        if (isfinite(cpu[k]) && vmaf_test_identical_f64(cpu[k], gpu[k])) {
            continue;
        }
        mismatches++;
        (void)fprintf(stderr, "\n%s %ux%u %u-bit %s: cpu=%.17g metal=%.17g delta=%.3e\n", c->name,
                      c->geometry.w, c->geometry.h, c->bpc, c->keys[k], cpu[k], gpu[k],
                      fabs(cpu[k] - gpu[k]));
    }
    return mismatches;
}

/* The case on each geometry of `list`; stops at the first one that differs. */
static unsigned list_mismatches(const AdmCase *base, const Geometry *list, size_t count)
{
    AdmCase c = *base;
    for (size_t i = 0; i < count; i++) {
        c.geometry = list[i];
        const unsigned mismatches = exact_mismatches(&c);
        if (mismatches || mu_skipped) {
            return mismatches;
        }
    }
    return 0u;
}

/* The scores and, with `debug=true`, the frame ratio and the sums it is formed
 * from. `suffix` is the feature-name suffix of the option set. */
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

/* The registered names of the default options, plus the debug outputs. */
#define DEFAULT_DEBUG_KEYS                                                                         \
    "VMAF_integer_feature_adm2_score", "VMAF_integer_feature_aim_score",                           \
        "VMAF_integer_feature_adm3_score", "integer_adm_scale0", "integer_adm_scale1",             \
        "integer_adm_scale2", "integer_adm_scale3", ADM_DEBUG_KEYS("")

#define DEBUG_OPT .n_opts = 1u, .opts = {{"debug", "true"}}
#define N_DEFAULT_DEBUG_KEYS 18u

/* The options of the default model `vmaf_v1.0.16_3d0h` for integer ADM. Every
 * one is a VMAF_OPT_FLAG_FEATURE_PARAM and non-default, so the feature-name
 * key both extractors emit is the fully suffixed form below. */
#define MODEL_SUFFIX "_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02_apn_2"

#define LIST_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Odd at every scale: 322x182 halves to 161x91, 81x46, 41x23 and 21x12. */
#define DEFAULT_CASE(name_, w_, h_, bpc_, sample_)                                                 \
    {                                                                                              \
        .name = (name_), .geometry = {(w_), (h_)}, .bpc = (bpc_), .sample = (sample_), DEBUG_OPT,  \
        .n_keys = N_DEFAULT_DEBUG_KEYS, .keys = {DEFAULT_DEBUG_KEYS}                               \
    }

static char *check_case(const AdmCase *c, char *message)
{
    mu_assert(message, exact_mismatches(c) == 0u);
    return NULL;
}

static char *test_adm_metal_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(TWIN_NAME);
    mu_assert("integer_adm_metal extractor must be registered", fex != NULL);
    mu_assert("integer_adm_metal name matches", !strcmp(fex->name, TWIN_NAME));
    return NULL;
}

/* Default options, every output including the per-scale sums. */
static char *test_adm_default_exact(void)
{
    static const AdmCase c = DEFAULT_CASE("adm", 256u, 144u, 8u, textured_sample);
    return check_case(&c, "integer_adm_metal differs from the CPU extractor");
}

static char *test_adm_10bit_exact(void)
{
    static const AdmCase c = DEFAULT_CASE("adm 10-bit", 256u, 144u, 10u, textured_sample);
    return check_case(&c, "integer_adm_metal differs from the CPU extractor at 10 bits");
}

static char *test_adm_odd_frame_exact(void)
{
    static const AdmCase c = DEFAULT_CASE("adm odd frame", 322u, 182u, 8u, textured_sample);
    return check_case(&c, "integer_adm_metal differs from the CPU extractor on an odd frame");
}

/* 3840x2160: the scale-0 denominator region exceeds 2^20 samples, so its row
 * sums are shifted before they are added (shift_accum = 1). */
static char *test_adm_2160p_exact(void)
{
    static const AdmCase c = DEFAULT_CASE("adm 2160p", 3840u, 2160u, 8u, textured_sample);
    return check_case(&c, "integer_adm_metal differs from the CPU extractor at 2160p");
}

/* Almost no reference detail: the denominator accumulators of the coarse
 * scales are small integers, and where their rows are rounded shows in
 * `integer_adm_den_scale2` and `_scale3`. */
static char *test_adm_sparse_detail_exact(void)
{
    static const AdmCase c = DEFAULT_CASE("adm sparse", 640u, 360u, 8u, sparse_sample);
    return check_case(&c, "integer_adm_metal differs from the CPU extractor on sparse detail");
}

/* 962x13542: the scale-0 denominator region is 387 x 5419 = 2^21 + 1 samples,
 * where the CPU's double logarithm gives a shift of 2 and a float logarithm
 * rounds to exactly 21 and gives 1. */
static char *test_adm_shift_boundary_area_exact(void)
{
    static const AdmCase c = DEFAULT_CASE("adm shift boundary", 962u, 13542u, 8u, textured_sample);
    return check_case(&c, "integer_adm_metal differs from the CPU extractor at the shift boundary");
}

static char *test_adm_model_options_exact(void)
{
    static const AdmCase c = {.name = "adm model options",
                              .geometry = {256u, 144u},
                              .bpc = 8u,
                              .sample = textured_sample,
                              .n_opts = 6u,
                              .opts = {{"adm_csf_mode", "2"},
                                       {"adm_dlm_weight", "0.7"},
                                       {"adm_enhn_gain_limit", "1.0"},
                                       {"adm_min_val", "0.5"},
                                       {"adm_noise_weight", "0.02"},
                                       {"adm_p_norm", "2.0"}},
                              .n_keys = 7u,
                              .keys = {ADM_SCORE_KEYS(MODEL_SUFFIX)}};
    return check_case(&c, "integer_adm_metal differs from the CPU under the default model options");
}

static char *test_adm_barten_mode_exact(void)
{
    static const AdmCase c = {.name = "adm Barten mode",
                              .geometry = {256u, 144u},
                              .bpc = 8u,
                              .sample = textured_sample,
                              .n_opts = 1u,
                              .opts = {{"adm_csf_mode", "1"}},
                              .n_keys = 7u,
                              .keys = {ADM_SCORE_KEYS("_csf_1")}};
    return check_case(&c, "integer_adm_metal differs from the CPU in Barten mode");
}

/* adm_skip_scale0: the CPU leaves the scale-0 numerator at 0 and seeds its
 * denominator with 1e-10 narrowed to float, and both enter the frame sums. */
static char *test_adm_skip_scale0_exact(void)
{
    static const AdmCase c = {.name = "adm skip scale 0",
                              .geometry = {256u, 144u},
                              .bpc = 8u,
                              .sample = textured_sample,
                              .n_opts = 2u,
                              .opts = {{"adm_skip_scale0", "true"}, {"debug", "true"}},
                              .n_keys = 18u,
                              .keys = {ADM_SCORE_KEYS("_ssz"), ADM_DEBUG_KEYS("_ssz")}};
    return check_case(&c, "integer_adm_metal differs from the CPU with adm_skip_scale0");
}

/* The default-option keys of the tiny-frame family, against the scalar CPU. */
#define TINY_CASE(name_, bpc_, sample_)                                                            \
    {.name = (name_),                                                                              \
     .bpc = (bpc_),                                                                                \
     .sample = (sample_),                                                                          \
     .scalar = true,                                                                               \
     .n_keys = 7u,                                                                                 \
     .keys = {"integer_adm_scale0", "integer_adm_scale1", "integer_adm_scale2",                    \
              "integer_adm_scale3", "VMAF_integer_feature_adm2_score",                             \
              "VMAF_integer_feature_aim_score", "VMAF_integer_feature_adm3_score"}}

/* One dimension from 17 to 32 in every row, so the scale-0 band is 9 to 16
 * samples wide or high; 32x32 is the size that scored NaN on the CUDA and HIP
 * twins. */
static char *test_adm_tiny_frame_exact(void)
{
    static const AdmCase c = TINY_CASE("adm tiny frame", 8u, ramp_sample);
    static const Geometry list[] = {{17u, 17u}, {18u, 18u}, {24u, 24u}, {32u, 32u},
                                    {20u, 64u}, {64u, 20u}, {31u, 48u}};
    mu_assert("integer_adm_metal differs from the scalar CPU on a tiny frame",
              list_mismatches(&c, list, LIST_LEN(list)) == 0u);
    return NULL;
}

/* Independent full-range noise reaches the int16 wrap of the scale-0 masking
 * threshold (T-SYCL-ADM-INT16-SEMANTICS-2026-09-18). */
static char *test_adm_full_range_noise_exact(void)
{
    static const AdmCase c = TINY_CASE("adm noise", 8u, noise_sample);
    static const Geometry list[] = {{96u, 64u}, {576u, 324u}};
    mu_assert("integer_adm_metal differs from the scalar CPU on full-range noise",
              list_mismatches(&c, list, LIST_LEN(list)) == 0u);
    return NULL;
}

/* 16-bit samples whose scale-0 vertical sums pass INT32_MAX. */
static char *test_adm_bright_16bit_exact(void)
{
    static const AdmCase c = TINY_CASE("adm bright 16-bit", 16u, bright16_sample);
    static const Geometry list[] = {{96u, 64u}, {576u, 324u}};
    mu_assert("integer_adm_metal differs from the scalar CPU on bright 16-bit input",
              list_mismatches(&c, list, LIST_LEN(list)) == 0u);
    return NULL;
}

/* A flat reference against isolated patches: the content that told a narrowed
 * masking centre tap from an unnarrowed one (ADR-1402). */
static char *test_adm_isolated_patch_exact(void)
{
    static const AdmCase c = TINY_CASE("adm isolated patches", 8u, patch_sample);
    static const Geometry list[] = {{24u, 24u}, {64u, 64u}, {176u, 144u}};
    mu_assert("integer_adm_metal differs from the scalar CPU on isolated patches",
              list_mismatches(&c, list, LIST_LEN(list)) == 0u);
    return NULL;
}

/* The enhanced-contrast picture at one fractional gain limit, scalar CPU. The
 * keys carry the limit as the feature-name suffix. */
#define GAIN_CASE(name_, limit_, suffix_)                                                          \
    {                                                                                              \
        .name = (name_), .bpc = 8u, .sample = enhanced_sample, .scalar = true, .n_opts = 2u,       \
        .opts = {{"adm_enhn_gain_limit", (limit_)}, {"debug", "true"}}, .n_keys = 18u,             \
        .keys = {ADM_SCORE_KEYS(suffix_), ADM_DEBUG_KEYS(suffix_)}                                 \
    }

static const Geometry GAIN_GEOMETRIES[] = {{96u, 64u}, {576u, 324u}};

/* The comparison only means something where the limit binds: the scalar CPU's
 * scale-0 score must move with it. */
static char *gain_limit_binds(const Geometry g)
{
    static const AdmCase at_1_2 = GAIN_CASE("adm egl 1.2", "1.2", "_egl_1.2");
    static const AdmCase at_1_5 = GAIN_CASE("adm egl 1.5", "1.5", "_egl_1.5");
    AdmCase low = at_1_2;
    AdmCase high = at_1_5;
    double s_low[MAX_KEYS] = {0.0};
    double s_high[MAX_KEYS] = {0.0};
    low.geometry = g;
    high.geometry = g;
    mu_assert("scalar CPU run at limit 1.2 failed", !adm_scores(NULL, &low, s_low));
    mu_assert("scalar CPU run at limit 1.5 failed", !adm_scores(NULL, &high, s_high));
    mu_assert("the gain limit does not bind on the enhanced-contrast picture",
              fabs(s_low[3] - s_high[3]) > 0.01);
    return NULL;
}

/* The twin forms the restored sample times the limit in binary32; the CPU
 * truncates the double product (ADR-1413). At 1.2 the two differ by one unit
 * on samples whose product lies next to an integer. */
static char *test_adm_gain_limit_1_2_exact(void)
{
    static const AdmCase c = GAIN_CASE("adm egl 1.2", "1.2", "_egl_1.2");
    for (size_t i = 0; i < LIST_LEN(GAIN_GEOMETRIES); i++) {
        mu_assert_msg(gain_limit_binds(GAIN_GEOMETRIES[i]));
    }
    mu_assert("integer_adm_metal differs from the scalar CPU at adm_enhn_gain_limit 1.2",
              list_mismatches(&c, GAIN_GEOMETRIES, LIST_LEN(GAIN_GEOMETRIES)) == 0u);
    return NULL;
}

static char *test_adm_gain_limit_1_5_exact(void)
{
    static const AdmCase c = GAIN_CASE("adm egl 1.5", "1.5", "_egl_1.5");
    for (size_t i = 0; i < LIST_LEN(GAIN_GEOMETRIES); i++) {
        mu_assert_msg(gain_limit_binds(GAIN_GEOMETRIES[i]));
    }
    mu_assert("integer_adm_metal differs from the scalar CPU at adm_enhn_gain_limit 1.5",
              list_mismatches(&c, GAIN_GEOMETRIES, LIST_LEN(GAIN_GEOMETRIES)) == 0u);
    return NULL;
}

/* A request for the extractor by name on a frame below the 17x17 minimum:
 * init() answers -EINVAL, which vmaf_read_pictures() returns. */
static int rejected_status(void *state, Geometry g, bool *setup_failed)
{
    static const AdmCase base = TINY_CASE("adm rejected", 8u, ramp_sample);
    AdmCase c = base;
    c.geometry = g;
    VmafContext *vmaf = NULL;
    const int err = adm_context(&vmaf, state, &c);
    *setup_failed = err != 0;
    int status = err;
    if (!err) {
        /* Only this call runs init(); feed_frame() fills both pictures. */
        status = feed_frame(vmaf, &c);
    }
    if (vmaf) {
        (void)vmaf_close(vmaf);
    }
    return status;
}

static char *test_adm_rejects_below_min_dim(void)
{
    static const Geometry rejected[] = {{8u, 8u}, {16u, 16u}, {16u, 17u}, {17u, 16u}};
    void *state = metal_device();
    if (!state) {
        return NULL;
    }
    int rc = -EINVAL;
    size_t bad = 0u;
    bool setup_failed = false;
    for (size_t i = 0; i < LIST_LEN(rejected) && rc == -EINVAL && !setup_failed; i++) {
        rc = rejected_status(state, rejected[i], &setup_failed);
        bad = i;
    }
    (void)metal_twin_close(state);
    mu_assert("rejection case: context setup failed (not init's refusal)", !setup_failed);
    if (rc != -EINVAL) {
        (void)fprintf(stderr, "\n%ux%u: returned %d\n", rejected[bad].w, rejected[bad].h, rc);
    }
    mu_assert("integer_adm_metal must reject frames below 17x17 with -EINVAL", rc == -EINVAL);
    return NULL;
}

static void run_default_cases(void)
{
    metal_run_case(test_adm_default_exact);
    metal_run_case(test_adm_10bit_exact);
    metal_run_case(test_adm_odd_frame_exact);
    metal_run_case(test_adm_2160p_exact);
    metal_run_case(test_adm_sparse_detail_exact);
    metal_run_case(test_adm_shift_boundary_area_exact);
}

/* ADR-2795: one context at two viewing distances, with `debug` at 8 and 10
 * bits: the first distance keeps its names and its debug scores, the second
 * files its seven under the names `adm_norm_view_dist=5` gives them. */
#define TWO_VIEW_CASE(name_, bpc_)                                                                 \
    {                                                                                              \
        .name = (name_), .geometry = {256u, 144u}, .bpc = (bpc_), .sample = textured_sample,       \
        .n_opts = 2u, .opts = {{"debug", "true"}, {"adm_norm_view_dist_extra", "5"}},              \
        .n_keys = N_DEFAULT_DEBUG_KEYS + 7u,                                                       \
        .keys = {DEFAULT_DEBUG_KEYS, ADM_SCORE_KEYS("_nvd_5")}                                     \
    }

#define MODEL_OPTS_6                                                                               \
    {"adm_csf_mode", "2"}, {"adm_dlm_weight", "0.7"}, {"adm_enhn_gain_limit", "1.0"},              \
        {"adm_min_val", "0.5"}, {"adm_noise_weight", "0.02"}, {"adm_p_norm", "2.0"}
#define MODEL_SUFFIX_5H "_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02_nvd_5_apn_2"

static char *test_adm_two_views_exact(void)
{
    static const AdmCase c8 = TWO_VIEW_CASE("adm two views", 8u);
    static const AdmCase c10 = TWO_VIEW_CASE("adm two views 10-bit", 10u);
    static const AdmCase model = {
        .name = "adm two views model options",
        .geometry = {256u, 144u},
        .bpc = 8u,
        .sample = textured_sample,
        .n_opts = 7u,
        .opts = {MODEL_OPTS_6, {"adm_norm_view_dist_extra", "5"}},
        .n_keys = 14u,
        .keys = {ADM_SCORE_KEYS(MODEL_SUFFIX), ADM_SCORE_KEYS(MODEL_SUFFIX_5H)}};
    char *msg = check_case(&c8, "integer_adm_metal differs from the CPU at two viewing distances");
    if (!msg) {
        msg = check_case(&c10, "integer_adm_metal differs from the CPU at two distances, 10-bit");
    }
    return msg ?
               msg :
               check_case(&model,
                          "integer_adm_metal differs from the CPU at two distances, model options");
}

/* ADR-2795: the twin registered at 3H and at 5H (the registry folds the
 * second into the first, as two models do) scores as one CPU context with
 * both distances. */
static char *test_adm_merged_registrations_exact(void)
{
    static const AdmCase c = {
        .name = "adm merged registrations",
        .geometry = {256u, 144u},
        .bpc = 8u,
        .sample = textured_sample,
        .n_opts = 6u,
        .opts = {MODEL_OPTS_6},
        .n_keys = 14u,
        .keys = {ADM_SCORE_KEYS(MODEL_SUFFIX), ADM_SCORE_KEYS(MODEL_SUFFIX_5H)},
        .merge_nvd = "5"};
    return check_case(&c, "integer_adm_metal registered at 3H and 5H differs from the CPU");
}

static void run_option_cases(void)
{
    metal_run_case(test_adm_model_options_exact);
    metal_run_case(test_adm_barten_mode_exact);
    metal_run_case(test_adm_skip_scale0_exact);
    metal_run_case(test_adm_two_views_exact);
    metal_run_case(test_adm_merged_registrations_exact);
}

static void run_content_cases(void)
{
    metal_run_case(test_adm_tiny_frame_exact);
    metal_run_case(test_adm_full_range_noise_exact);
    metal_run_case(test_adm_bright_16bit_exact);
    metal_run_case(test_adm_isolated_patch_exact);
}

static void run_gain_limit_cases(void)
{
    metal_run_case(test_adm_gain_limit_1_2_exact);
    metal_run_case(test_adm_gain_limit_1_5_exact);
    metal_run_case(test_adm_rejects_below_min_dim);
}

char *run_tests(void)
{
    metal_run_case(test_adm_metal_registered);
    run_default_cases();
    run_option_cases();
    run_content_cases();
    run_gain_limit_cases();
    return metal_first_failure;
}

/* NOLINTEND(modernize-use-nullptr) */
