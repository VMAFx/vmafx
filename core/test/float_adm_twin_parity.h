/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_adm CPU vs. GPU twin: the fixtures, the cases and the bit-for-bit
 * comparison a twin's parity test wraps (ADR-1434 for SYCL; the cases are
 * those of test_cuda_float_adm_parity.c, ADR-1420).
 *
 * A twin that runs adm_tools.c's arithmetic operation for operation, adds the
 * rows in the reference's order and concludes with the reference's own
 * routines returns the CPU extractor's doubles, so the comparison is
 * equality: every output of every exact case has the CPU's bits, the
 * per-scale numerators and denominators of `debug=true` included. The cases:
 *
 *   - the default options on four fixtures (a textured ramp, independent
 *     noise, a contrast change, an isolated sample) at 8, 10, 12 and 16 bit;
 *   - frames that are odd at every scale, 17x17 (the smallest frame whose
 *     coarsest bands still have two samples a side), a narrow frame and
 *     1920x1080;
 *   - the options a model or a user sets: `adm_enhn_gain_limit` at a value
 *     that is not an fp32 value, `adm_bypass_cm`, `adm_skip_aim_scale`,
 *     `adm_skip_scale0`, `adm_norm_view_dist`, the per-scale CSF weight
 *     overrides and `adm_csf_scale` (a no-op in the Watson mode);
 *   - a frame whose sums lie below the floor the twins once applied.
 *
 * One option is not exact: adm_p_norm other than 1 or 3 raises each term
 * with powf(), and a device's powf is not glibc's. That case keeps a
 * tolerance.
 *
 * A test describes its backend in one AdmTwin and wraps the adm_twin_*()
 * cases its twin supports. Each comparison opens its own device state.
 * Without a device a case is skipped and the test exits 77.
 */

#ifndef LIBVMAF_TEST_FLOAT_ADM_TWIN_PARITY_H_
#define LIBVMAF_TEST_FLOAT_ADM_TWIN_PARITY_H_

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "float_bits.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): included by C translation units. The
 * fork builds C as C23, where clang-tidy proposes `nullptr`, but the required
 * MSVC C build does not provide that keyword. Preserve the portable C
 * spelling. ADR-1138. */

/* Fixture geometry — large enough for the 4-scale ADM DWT pyramid. */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif

#define ADM_TWIN_MAX_KEYS 18u

/* A device's powf() against glibc's, through the p-norm root and the score
 * ratio. Measured on the Netflix pair and both checkerboard pairs at
 * adm_p_norm 2 and 2.5: 1.9e-7 at most on an Arc A380, 1.1e-7 on an
 * RTX 4090. */
#define ADM_TWIN_P_NORM_TOL 1e-6

/* One GPU backend's twin, as the shared cases drive it. */
typedef struct AdmTwin {
    const char *extractor; /* registry name, e.g. "float_adm_sycl" */
    const char *backend;   /* for messages, e.g. "SYCL" */
    /* Opens a device state. Non-zero: no device, the case is skipped. */
    int (*open)(void **state);
    int (*import)(VmafContext *vmaf, void *state);
    int (*close)(void *state);
} AdmTwin;

typedef enum AdmTwinContent {
    ADM_TWIN_TEXTURE,  /* a textured ramp with a position-dependent error */
    ADM_TWIN_NOISE,    /* independent hashes: every band and angle populated */
    ADM_TWIN_CONTRAST, /* the reference with its contrast raised by a quarter */
    ADM_TWIN_ISOLATED, /* a flat frame; the reference has one sample one level up */
} AdmTwinContent;

/* One case: a frame, an option (NULL for none) and the keys to compare. */
typedef struct AdmTwinCase {
    const char *what;
    unsigned w;
    unsigned h;
    unsigned bpc;
    AdmTwinContent content;
    const char *option;
    const char *value;
    bool debug;
    const char *const *keys;
    size_t count;
} AdmTwinCase;

/* Deterministic position hash. */
static inline unsigned adm_twin_hash(unsigned row, unsigned col, unsigned salt)
{
    unsigned x = row * 73856093u ^ col * 19349663u ^ salt * 83492791u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return x;
}

static inline unsigned adm_twin_textured_luma(unsigned row, unsigned col, bool distorted)
{
    unsigned v = ((row * 3u + col * 2u) & 0xFFu) ^ (((row >> 2) * (col >> 3)) & 0x1Fu);
    if (distorted)
        v += 9u + ((row * 5u + col * 7u) % 11u);
    return v & 0xFFu;
}

static inline unsigned adm_twin_noise_luma(unsigned row, unsigned col, bool distorted)
{
    const unsigned base = 64u + (adm_twin_hash(row >> 1, col >> 1, 3u) & 0x7Fu);
    if (!distorted)
        return base;
    return base + (adm_twin_hash(row, col, 4u) % 17u);
}

/* A reference with detail and the same picture at 1.25 times the contrast:
 * every band of the distorted picture is the reference's times 1.25, so the
 * decouple's angle test passes everywhere and the restored signal is bounded
 * by the enhancement gain limit. */
static inline unsigned adm_twin_contrast_luma(unsigned row, unsigned col, bool distorted)
{
    const int detail = (int)(adm_twin_hash(row, col, 5u) % 81u) - 40;
    return (unsigned)(128 + (distorted ? (detail * 5) / 4 : detail));
}

/* Luma in units of the bit depth's least significant level for the isolated
 * fixture, in units of an 8-bit level otherwise. */
static inline unsigned adm_twin_luma(const AdmTwinCase *c, unsigned row, unsigned col,
                                     bool distorted)
{
    const unsigned gain = 1u << (c->bpc - 8u);
    switch (c->content) {
    case ADM_TWIN_NOISE:
        return adm_twin_noise_luma(row, col, distorted) * gain;
    case ADM_TWIN_CONTRAST:
        return adm_twin_contrast_luma(row, col, distorted) * gain;
    case ADM_TWIN_ISOLATED:
        return 128u * gain + ((!distorted && row == c->h / 2u && col == c->w / 2u) ? 1u : 0u);
    case ADM_TWIN_TEXTURE:
    default:
        return adm_twin_textured_luma(row, col, distorted) * gain;
    }
}

static inline void adm_twin_put_sample(VmafPicture *pic, unsigned plane, unsigned row, unsigned col,
                                       unsigned v)
{
    const unsigned peak = (1u << pic->bpc) - 1u;
    uint8_t *line = (uint8_t *)pic->data[plane] + (size_t)row * (size_t)pic->stride[plane];
    if (pic->bpc <= 8u) {
        line[col] = (uint8_t)(v > peak ? peak : v);
    } else {
        ((uint16_t *)line)[col] = (uint16_t)(v > peak ? peak : v);
    }
}

static inline int adm_twin_fill_picture(VmafPicture *pic, const AdmTwinCase *c, bool distorted)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, c->bpc, c->w, c->h);
    if (err)
        return err;
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++)
            adm_twin_put_sample(pic, 0u, row, col, adm_twin_luma(c, row, col, distorted));
    }
    for (unsigned p = 1; p < 3; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++)
                adm_twin_put_sample(pic, p, row, col, 128u << (c->bpc - 8u));
        }
    }
    return 0;
}

/* The case's options; NULL for none (and when a set fails, which the
 * extractor then reports as a missing key). */
static inline VmafFeatureDictionary *adm_twin_opts(const AdmTwinCase *c)
{
    VmafFeatureDictionary *d = NULL;
    if (c->debug && vmaf_feature_dictionary_set(&d, "debug", "true"))
        return NULL;
    if (c->option && vmaf_feature_dictionary_set(&d, c->option, c->value)) {
        (void)vmaf_feature_dictionary_free(&d);
        return NULL;
    }
    return d;
}

/* Feed the case's frame, flush, and read its keys. */
static inline mu_message_t adm_twin_read(VmafContext *vmaf, const AdmTwinCase *c, double *out)
{
    VmafPicture ref;
    VmafPicture dist;
    mu_assert("fill reference failed", !adm_twin_fill_picture(&ref, c, false));
    mu_assert("fill distorted failed", !adm_twin_fill_picture(&dist, c, true));
    mu_assert("vmaf_read_pictures failed", !vmaf_read_pictures(vmaf, &ref, &dist, 0u));
    mu_assert("vmaf_read_pictures(EOS) failed", !vmaf_read_pictures(vmaf, NULL, NULL, 0));
    for (size_t k = 0; k < c->count; k++) {
        if (vmaf_feature_score_at_index(vmaf, c->keys[k], &out[k], 0u)) {
            (void)fprintf(stderr, "\nmissing feature-name key: %s (%s)\n", c->keys[k], c->what);
            return "float ADM feature-name key not emitted";
        }
    }
    return NULL;
}

/* One frame of the case on the CPU (`state` NULL) or on the twin. */
static inline mu_message_t adm_twin_score(const AdmTwin *twin, void *state, const AdmTwinCase *c,
                                          double *out)
{
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    mu_assert("vmaf_init failed", !vmaf_init(&vmaf, cfg));
    mu_message_t msg = NULL;
    if (state && twin->import(vmaf, state))
        msg = "importing the device state failed";
    /* vmaf_use_feature() takes the dictionary over, on failure too. */
    if (!msg && vmaf_use_feature(vmaf, state ? twin->extractor : "float_adm", adm_twin_opts(c)))
        msg = "vmaf_use_feature failed";
    if (!msg)
        msg = adm_twin_read(vmaf, c, out);
    if (vmaf_close(vmaf) != 0 && !msg)
        msg = "vmaf_close failed";
    return msg;
}

/* Every key of the case within `tol` of the CPU on the twin; tol = 0 is
 * equality. `cpu_out` (may be NULL) receives the CPU's values. Without a
 * device the case is skipped. */
static inline mu_message_t adm_twin_check(const AdmTwin *twin, const AdmTwinCase *c, double tol,
                                          double *cpu_out)
{
    double cpu[ADM_TWIN_MAX_KEYS];
    double gpu[ADM_TWIN_MAX_KEYS];
    mu_assert("adm_twin_check: too many keys", c->count <= ADM_TWIN_MAX_KEYS);
    mu_message_t msg = adm_twin_score(twin, NULL, c, cpu);
    if (msg)
        return msg;
    if (cpu_out)
        memcpy(cpu_out, cpu, c->count * sizeof(double));
    void *state = NULL;
    if (twin->open(&state) != 0 || state == NULL) {
        (void)fprintf(stderr, "[skip: no %s device] ", twin->backend);
        mu_skipped = 1;
        return NULL;
    }
    msg = adm_twin_score(twin, state, c, gpu);
    const int close_err = twin->close(state);
    if (msg)
        return msg;
    mu_assert("closing the device state failed", close_err == 0);
    unsigned mismatches = 0u;
    for (size_t k = 0; k < c->count; k++) {
        mu_assert("CPU float_adm output is non-finite", isfinite(cpu[k]));
        const bool within =
            tol == 0.0 ? vmaf_test_identical_f64(cpu[k], gpu[k]) : fabs(cpu[k] - gpu[k]) <= tol;
        if (within)
            continue;
        mismatches++;
        (void)fprintf(stderr, "\n%s %ux%u %u-bit %s: cpu=%.17g %s=%.17g delta=%.3e\n", c->what,
                      c->w, c->h, c->bpc, c->keys[k], cpu[k], twin->backend, gpu[k],
                      fabs(cpu[k] - gpu[k]));
    }
    mu_assert("the float_adm twin differs from the CPU extractor", mismatches == 0u);
    return NULL;
}

/* The seven scores under an option's feature-name suffix, and the sums
 * `debug=true` adds. The debug ratio `adm` is filed without the suffix on the
 * CPU and on every twin (ADR-2056), so option cases end with it unsuffixed. */
#define ADM_TWIN_SCORE_KEYS(suffix)                                                                \
    "adm2" suffix, "aim" suffix, "adm3" suffix, "adm_scale0" suffix, "adm_scale1" suffix,          \
        "adm_scale2" suffix, "adm_scale3" suffix
#define ADM_TWIN_SUM_KEYS(suffix)                                                                  \
    "adm_num" suffix, "adm_den" suffix, "adm_num_scale0" suffix, "adm_den_scale0" suffix,          \
        "adm_num_scale1" suffix, "adm_den_scale1" suffix, "adm_num_scale2" suffix,                 \
        "adm_den_scale2" suffix, "adm_num_scale3" suffix, "adm_den_scale3" suffix

static const char *const ADM_TWIN_DEBUG_KEYS[] = {
    "VMAF_feature_adm2_score",       "VMAF_feature_aim_score",
    "VMAF_feature_adm3_score",       "VMAF_feature_adm_scale0_score",
    "VMAF_feature_adm_scale1_score", "VMAF_feature_adm_scale2_score",
    "VMAF_feature_adm_scale3_score", "adm",
    ADM_TWIN_SUM_KEYS(""),
};
#define ADM_TWIN_NUM_DEBUG_KEYS (sizeof(ADM_TWIN_DEBUG_KEYS) / sizeof(ADM_TWIN_DEBUG_KEYS[0]))
/* The seven scores and the ten sums of an option case, then the unsuffixed `adm`. */
#define ADM_TWIN_NUM_OPTION_KEYS 18u
#define ADM_TWIN_NUM_SCORE_KEYS 7u

/* Default options with `debug=true`: every output, the per-scale sums
 * included. */
static inline mu_message_t adm_twin_default(const AdmTwin *twin, const char *what, unsigned w,
                                            unsigned h, unsigned bpc, AdmTwinContent content)
{
    const AdmTwinCase c = {.what = what,
                           .w = w,
                           .h = h,
                           .bpc = bpc,
                           .content = content,
                           .debug = true,
                           .keys = ADM_TWIN_DEBUG_KEYS,
                           .count = ADM_TWIN_NUM_DEBUG_KEYS};
    return adm_twin_check(twin, &c, 0.0, NULL);
}

/* One option on the build's fixture with `debug=true`; `keys` are the
 * ADM_TWIN_NUM_OPTION_KEYS keys under the option's suffix. */
static inline mu_message_t adm_twin_option(const AdmTwin *twin, const char *what,
                                           AdmTwinContent content, const char *option,
                                           const char *value, const char *const *keys)
{
    const AdmTwinCase c = {.what = what,
                           .w = FIXTURE_W,
                           .h = FIXTURE_H,
                           .bpc = 8u,
                           .content = content,
                           .option = option,
                           .value = value,
                           .debug = true,
                           .keys = keys,
                           .count = ADM_TWIN_NUM_OPTION_KEYS};
    return adm_twin_check(twin, &c, 0.0, NULL);
}

static inline mu_message_t adm_twin_registered(const AdmTwin *twin)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(twin->extractor);
    mu_assert("the float_adm twin must be registered", fex != NULL);
    mu_assert("the float_adm twin's name matches", !strcmp(fex->name, twin->extractor));
    return NULL;
}

static inline mu_message_t adm_twin_default_exact(const AdmTwin *twin)
{
    return adm_twin_default(twin, "float_adm", FIXTURE_W, FIXTURE_H, 8u, ADM_TWIN_TEXTURE);
}

static inline mu_message_t adm_twin_noise_exact(const AdmTwin *twin)
{
    return adm_twin_default(twin, "float_adm noise", FIXTURE_W, FIXTURE_H, 8u, ADM_TWIN_NOISE);
}

static inline mu_message_t adm_twin_10bit_exact(const AdmTwin *twin)
{
    return adm_twin_default(twin, "float_adm", FIXTURE_W, FIXTURE_H, 10u, ADM_TWIN_NOISE);
}

static inline mu_message_t adm_twin_12bit_exact(const AdmTwin *twin)
{
    return adm_twin_default(twin, "float_adm", FIXTURE_W, FIXTURE_H, 12u, ADM_TWIN_TEXTURE);
}

static inline mu_message_t adm_twin_16bit_exact(const AdmTwin *twin)
{
    return adm_twin_default(twin, "float_adm", FIXTURE_W, FIXTURE_H, 16u, ADM_TWIN_NOISE);
}

/* Odd at every scale: 322x182 halves to 161x91, 81x46, 41x23 and 21x12. */
static inline mu_message_t adm_twin_odd_frame_exact(const AdmTwin *twin)
{
    return adm_twin_default(twin, "float_adm odd", 322u, 182u, 8u, ADM_TWIN_NOISE);
}

/* 17x17 is the smallest frame whose coarsest bands still have two samples a
 * side (9, 5, 3, 2). The reduced region of every scale reaches the band's
 * edges there, so the mirrored and clamped taps of the masking threshold are
 * part of every sum. */
static inline mu_message_t adm_twin_smallest_frame_exact(const AdmTwin *twin)
{
    return adm_twin_default(twin, "float_adm smallest", 17u, 17u, 8u, ADM_TWIN_NOISE);
}

static inline mu_message_t adm_twin_narrow_frame_exact(const AdmTwin *twin)
{
    return adm_twin_default(twin, "float_adm narrow", 18u, 131u, 8u, ADM_TWIN_NOISE);
}

/* A scale-0 row of the reduced region is 768 samples and there are 432 of
 * them: longer than a sub-group, a work-group or a tile of any reduction. */
static inline mu_message_t adm_twin_1080p_exact(const AdmTwin *twin)
{
    return adm_twin_default(twin, "float_adm 1080p", 1920u, 1080u, 8u, ADM_TWIN_NOISE);
}

/* The enhancement gain limit is a double the reference multiplies in fp64:
 * 1.2 is not an fp32 value, and with the contrast fixture the limited product
 * is the restored signal of most samples. */
static inline mu_message_t adm_twin_gain_limit_exact(const AdmTwin *twin)
{
    static const char *const keys[] = {ADM_TWIN_SCORE_KEYS("_egl_1.2"),
                                       ADM_TWIN_SUM_KEYS("_egl_1.2"), "adm"};
    return adm_twin_option(twin, "float_adm egl=1.2", ADM_TWIN_CONTRAST, "adm_enhn_gain_limit",
                           "1.2", keys);
}

static inline mu_message_t adm_twin_bypass_cm_exact(const AdmTwin *twin)
{
    static const char *const keys[] = {ADM_TWIN_SCORE_KEYS("_bcm_1"), ADM_TWIN_SUM_KEYS("_bcm_1"),
                                       "adm"};
    return adm_twin_option(twin, "float_adm bcm=1", ADM_TWIN_NOISE, "adm_bypass_cm", "1", keys);
}

static inline mu_message_t adm_twin_skip_aim_scale_exact(const AdmTwin *twin)
{
    static const char *const keys[] = {ADM_TWIN_SCORE_KEYS("_sasc_1"), ADM_TWIN_SUM_KEYS("_sasc_1"),
                                       "adm"};
    return adm_twin_option(twin, "float_adm sasc=1", ADM_TWIN_NOISE, "adm_skip_aim_scale", "1",
                           keys);
}

/* adm_skip_scale0: scale 0 contributes a zero numerator and the reference's
 * 1e-10 denominator, and its score is reported as 0. */
static inline mu_message_t adm_twin_skip_scale0_exact(const AdmTwin *twin)
{
    static const char *const keys[] = {ADM_TWIN_SCORE_KEYS("_ssz"), ADM_TWIN_SUM_KEYS("_ssz"),
                                       "adm"};
    const AdmTwinCase c = {.what = "float_adm ssz",
                           .w = FIXTURE_W,
                           .h = FIXTURE_H,
                           .bpc = 8u,
                           .content = ADM_TWIN_NOISE,
                           .option = "adm_skip_scale0",
                           .value = "true",
                           .debug = true,
                           .keys = keys,
                           .count = ADM_TWIN_NUM_OPTION_KEYS};
    double cpu[ADM_TWIN_MAX_KEYS];
    mu_message_t msg = adm_twin_check(twin, &c, 0.0, cpu);
    if (msg)
        return msg;
    /* keys: [3] adm_scale0, [9] adm_num_scale0. */
    mu_assert("a skipped scale 0 scores 0", cpu[3] == 0.0 && cpu[9] == 0.0);
    return NULL;
}

static inline mu_message_t adm_twin_view_dist_exact(const AdmTwin *twin)
{
    static const char *const keys[] = {ADM_TWIN_SCORE_KEYS("_nvd_1.5"),
                                       ADM_TWIN_SUM_KEYS("_nvd_1.5"), "adm"};
    return adm_twin_option(twin, "float_adm nvd=1.5", ADM_TWIN_NOISE, "adm_norm_view_dist", "1.5",
                           keys);
}

/* The per-scale CSF weight overrides: `adm_f1s0` replaces the weight of the
 * (h, v) bands of scale 0, `adm_f2s2` that of the (d) band of scale 2. */
static inline mu_message_t adm_twin_weight_overrides_exact(const AdmTwin *twin)
{
    static const char *const f1[] = {ADM_TWIN_SCORE_KEYS("_f1s0_0.5"),
                                     ADM_TWIN_SUM_KEYS("_f1s0_0.5"), "adm"};
    static const char *const f2[] = {ADM_TWIN_SCORE_KEYS("_f2s2_1.75"),
                                     ADM_TWIN_SUM_KEYS("_f2s2_1.75"), "adm"};
    mu_message_t msg =
        adm_twin_option(twin, "float_adm f1s0=0.5", ADM_TWIN_NOISE, "adm_f1s0", "0.5", f1);
    if (msg)
        return msg;
    return adm_twin_option(twin, "float_adm f2s2=1.75", ADM_TWIN_NOISE, "adm_f2s2", "1.75", f2);
}

/* ADR-1214 — adm_csf_scale is a no-op in the Watson-97 mode the twins
 * implement, exactly as it is on the CPU (adm_csf_rfactor_s() consults it
 * only in Barten mode), and it files the scores under the CPU's `scf` alias. */
static inline mu_message_t adm_twin_csf_scale_is_a_watson_mode_noop(const AdmTwin *twin)
{
    static const char *const keys[] = {ADM_TWIN_SCORE_KEYS("_scf_2"), ADM_TWIN_SUM_KEYS("_scf_2"),
                                       "adm"};
    return adm_twin_option(twin, "float_adm scf=2", ADM_TWIN_NOISE, "adm_csf_scale", "2.0", keys);
}

/* adm_p_norm = 1: the terms are the samples themselves, as powf(x, 1) is. */
static inline mu_message_t adm_twin_p_norm_one_exact(const AdmTwin *twin)
{
    static const char *const keys[] = {ADM_TWIN_SCORE_KEYS("_apn_1"), ADM_TWIN_SUM_KEYS("_apn_1"),
                                       "adm"};
    return adm_twin_option(twin, "float_adm apn=1", ADM_TWIN_NOISE, "adm_p_norm", "1.0", keys);
}

/* ADR-1220 — adm_p_norm reaches the kernels. Not exact: both sides raise the
 * terms with powf(), and a device's is not glibc's. The scores only: the sums
 * are fp32 values in the hundreds, where one unit in the last place is
 * 1.5e-5. */
static inline mu_message_t adm_twin_p_norm_reaches_kernel(const AdmTwin *twin)
{
    static const char *const keys[] = {ADM_TWIN_SCORE_KEYS("_apn_2")};
    const AdmTwinCase c = {.what = "float_adm apn=2",
                           .w = FIXTURE_W,
                           .h = FIXTURE_H,
                           .bpc = 8u,
                           .content = ADM_TWIN_NOISE,
                           .option = "adm_p_norm",
                           .value = "2.0",
                           .keys = keys,
                           .count = ADM_TWIN_NUM_SCORE_KEYS};
    double cpu[ADM_TWIN_MAX_KEYS];
    double def[ADM_TWIN_MAX_KEYS];
    mu_message_t msg = adm_twin_check(twin, &c, ADM_TWIN_P_NORM_TOL, cpu);
    if (msg)
        return msg;
    /* The exponent is honoured, not merely accepted: the scores differ from
     * the default exponent's by far more than the tolerance. */
    const AdmTwinCase d = {.what = "float_adm",
                           .w = FIXTURE_W,
                           .h = FIXTURE_H,
                           .bpc = 8u,
                           .content = ADM_TWIN_NOISE,
                           .keys = ADM_TWIN_DEBUG_KEYS,
                           .count = ADM_TWIN_NUM_SCORE_KEYS};
    msg = adm_twin_check(twin, &d, 0.0, def);
    if (msg)
        return msg;
    mu_assert("adm_p_norm = 2 must change adm2", fabs(cpu[0] - def[0]) > 1e-4);
    return NULL;
}

/* A flat 16-bit frame whose reference has one sample one level up, scored
 * without the noise floor. The reference band energy is then about 3e-4 and
 * the distorted picture has none: the CPU reports adm2 = 0. A twin that
 * floors its frame sums at 1e-2 * area / 1080p, where the CPU floors them at
 * 1e-10 of that, zeroes the denominator and reports adm2 = 1. */
static inline mu_message_t adm_twin_small_sums_are_not_floored(const AdmTwin *twin)
{
    static const char *const keys[] = {ADM_TWIN_SCORE_KEYS("_nw_0"), ADM_TWIN_SUM_KEYS("_nw_0"),
                                       "adm"};
    const AdmTwinCase c = {.what = "float_adm isolated",
                           .w = 576u,
                           .h = 324u,
                           .bpc = 16u,
                           .content = ADM_TWIN_ISOLATED,
                           .option = "adm_noise_weight",
                           .value = "0",
                           .debug = true,
                           .keys = keys,
                           .count = ADM_TWIN_NUM_OPTION_KEYS};
    double cpu[ADM_TWIN_MAX_KEYS];
    mu_message_t msg = adm_twin_check(twin, &c, 0.0, cpu);
    if (msg)
        return msg;
    /* keys: [0] adm2, [8] adm_den. */
    mu_assert("the isolated fixture must leave a denominator below the old floor",
              cpu[8] > 0.0 && cpu[8] < 1e-2 * (576.0 * 324.0) / (1920.0 * 1080.0));
    mu_assert("the CPU reports adm2 = 0 for the isolated fixture", cpu[0] == 0.0);
    return NULL;
}

/* init() result of the twin for a w x h frame, without a device: the size
 * check comes before any device resource is claimed. */
static inline int adm_twin_init_result(const AdmTwin *twin, unsigned w, unsigned h)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(twin->extractor);
    if (!fex)
        return 1;
    void *priv = calloc(1, fex->priv_size);
    if (!priv)
        return 1;
    fex->priv = priv;
    const int err = fex->init(fex, VMAF_PIX_FMT_YUV420P, 8u, w, h);
    fex->priv = NULL;
    free(priv);
    return err;
}

/* Below 17x17 the scale-3 bands have one sample. The CPU float_adm refuses
 * such frames (-EINVAL from init), and so must the twin. */
static inline mu_message_t adm_twin_rejects_frames_below_17(const AdmTwin *twin)
{
    mu_assert("the twin must reject 8x8", adm_twin_init_result(twin, 8u, 8u) == -EINVAL);
    mu_assert("the twin must reject 16x16", adm_twin_init_result(twin, 16u, 16u) == -EINVAL);
    mu_assert("the twin must reject 17x16", adm_twin_init_result(twin, 17u, 16u) == -EINVAL);
    mu_assert("the twin must reject 16x17", adm_twin_init_result(twin, 16u, 17u) == -EINVAL);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* LIBVMAF_TEST_FLOAT_ADM_TWIN_PARITY_H_ */
