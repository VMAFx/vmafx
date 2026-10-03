/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * cambi CPU vs. Metal: the twin returns the CPU's score bit for bit.
 *
 * CAMBI (Contrast-Aware Multiscale Banding Index) is registered as `cambi` on
 * the CPU (core/src/feature/cambi.c) and as `integer_cambi_metal` on Metal;
 * both emit `Cambi_feature_cambi_score`. The CUDA, SYCL and HIP twins return
 * the CPU's score exactly (ADR-1357, ADR-1379, ADR-1378; the CUDA twin is
 * held to `==` by test_cuda_cambi_parity.c and test_cuda_exact_twins.c), so
 * this test asserts equality, not a tolerance, on every frame of every case.
 *
 * No state row records a Metal cambi defect: the test measures whether the
 * twin is exact. The default-option cases use the two fixtures of the CUDA
 * test: a quantised gradient (flat in the vertical direction, which leaves
 * the spatial-mask border, the mode filter's border rows and the vertical
 * tie-breaks unobservable) and a textured one with a vertical ramp, a
 * deterministic dither and an inverted border ring that makes them
 * observable. The option cases score the textured fixture with one option
 * each (the shipped model vmaf_v1.0.16_3d0h asks for the defaults): the
 * clipping value cambi_max_val, cambi_vis_lum_threshold, cambi_topk,
 * window_size, max_log_contrast and cambi_high_res_speedup, whose scores
 * appear under the suffixed feature name the option set derives, and 10-bit
 * input. A 1920x1080 frame runs the high-resolution speed-up for real.
 *
 * Each run feeds four frames, so the pipelining of submit() and collect() and
 * the per-frame reset of the twin's state are in the comparison.
 *
 * Skip behaviour: without a Metal device every comparison reports the skip
 * and the run exits 77. The registration case needs no device.
 */

#include "metal_twin.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "feature/feature_extractor.h"
#include "libvmaf/feature.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

static const char *const TWIN_NAME = METAL_TWIN("integer_cambi_metal", "cambi");

/* Frames per run. */
#define NUM_FRAMES 4u
#define DEFAULT_KEY "Cambi_feature_cambi_score"

/* One comparison: a fixture, at most one option, and the key of the score. */
typedef struct CambiCase {
    const char *name;
    unsigned w;
    unsigned h;
    unsigned bpc;
    bool textured;
    const char *option; /* NULL, or an option name */
    const char *value;  /* the option's value */
    const char *key;    /* the score under the name the option set derives */
} CambiCase;

static uint32_t fixture_lcg(uint32_t x)
{
    return x * 1664525u + 1013904223u;
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

/* Luma code (8-bit scale) of one sample. The gradient is horizontal 32-pixel
 * bands; the textured fixture adds 24-code bands, a 48-row vertical ramp, a
 * deterministic +1 dither on about 19% of the samples and an inverted border
 * ring, so the first and last row and column differ from their neighbours. */
static unsigned luma(const CambiCase *c, unsigned row, unsigned col, unsigned salt)
{
    if (!c->textured) {
        return (col / 32u) * 32u + salt * 4u;
    }
    unsigned v = (col / 32u) * 24u + (row / 48u) * 5u + salt * 3u;
    const uint32_t h = fixture_lcg(row * 8191u + col * 131u + salt * 7919u);
    if ((h >> 29) < 3u) {
        v += 1u;
    }
    if (row == 0u || row + 1u == c->h || col == 0u || col + 1u == c->w) {
        v += 17u;
    }
    return v;
}

static int fill_pic(VmafPicture *pic, const CambiCase *c, unsigned salt)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, c->bpc, c->w, c->h);
    if (err) {
        return err;
    }
    for (unsigned row = 0u; row < pic->h[0]; row++) {
        for (unsigned col = 0u; col < pic->w[0]; col++) {
            put_sample(pic, 0u, row, col, (luma(c, row, col, salt) & 0xFFu) << (c->bpc - 8u));
        }
    }
    /* CAMBI is luma-only. */
    for (unsigned p = 1u; p < 3u; p++) {
        for (unsigned row = 0u; row < pic->h[p]; row++) {
            for (unsigned col = 0u; col < pic->w[p]; col++) {
                put_sample(pic, p, row, col, 1u << (c->bpc - 1u));
            }
        }
    }
    return 0;
}

/* Feed NUM_FRAMES ref/dist pairs (the distorted salt varies per frame), then
 * flush with an EOS marker. */
static int feed_frames(VmafContext *vmaf, const CambiCase *c)
{
    for (unsigned frame = 0u; frame < NUM_FRAMES; frame++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_pic(&ref, c, 0u);
        if (err) {
            return err;
        }
        err = fill_pic(&dist, c, 1u + frame);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            return err;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, frame);
        if (err) {
            return err;
        }
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

/* A context with the CPU `cambi`, or with the twin on `state`, carrying the
 * case's option. */
static int cambi_context(VmafContext **vmaf, void *state, const CambiCase *c)
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    int err = vmaf_init(vmaf, cfg);
    if (!err && state) {
        err = metal_twin_import(*vmaf, state);
    }
    VmafFeatureDictionary *opts = NULL;
    if (!err && c->option) {
        err = vmaf_feature_dictionary_set(&opts, c->option, c->value);
    }
    if (!err) {
        /* vmaf_use_feature() takes the dictionary over, on failure too. */
        err = vmaf_use_feature(*vmaf, state ? TWIN_NAME : "cambi", opts);
    }
    return err;
}

/* The score of every frame through one extractor; returns the first error. */
static int cambi_scores(void *state, const CambiCase *c, double *out)
{
    VmafContext *vmaf = NULL;
    int err = cambi_context(&vmaf, state, c);
    if (!err) {
        err = feed_frames(vmaf, c);
    }
    for (unsigned frame = 0u; frame < NUM_FRAMES && !err; frame++) {
        err = vmaf_feature_score_at_index(vmaf, c->key, &out[frame], frame);
        if (err) {
            (void)fprintf(stderr, "\n%s: no score for %s (%s)\n", c->name, c->key,
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

/* The frames of the case whose twin score is not the CPU's, each one
 * reported; UINT32_MAX when a run failed. A skipped twin leg counts as 0.
 * `cpu_first` returns the CPU's first score. */
static unsigned exact_mismatches(const CambiCase *c, double *cpu_first)
{
    double cpu[NUM_FRAMES] = {0.0};
    double gpu[NUM_FRAMES] = {0.0};
    void *state = metal_device();
    if (!state) {
        return 0u;
    }
    const int gpu_err = cambi_scores(state, c, gpu);
    (void)metal_twin_close(state);
    const int cpu_err = gpu_err ? 0 : cambi_scores(NULL, c, cpu);
    if (gpu_err || cpu_err) {
        (void)fprintf(stderr, "\n%s: run failed (%s %d, cpu %d)\n", c->name, METAL_TWIN_BACKEND,
                      gpu_err, cpu_err);
        return UINT32_MAX;
    }
    unsigned mismatches = 0u;
    for (unsigned frame = 0u; frame < NUM_FRAMES; frame++) {
        if (isfinite(cpu[frame]) && cpu[frame] == gpu[frame]) {
            continue;
        }
        mismatches++;
        (void)fprintf(stderr, "\n%s %ux%u %u-bit frame %u %s: cpu=%.17g metal=%.17g\n", c->name,
                      c->w, c->h, c->bpc, frame, c->key, cpu[frame], gpu[frame]);
    }
    *cpu_first = cpu[0];
    return mismatches;
}

static char *check_case(const CambiCase *c, char *message)
{
    double cpu_first = 0.0;
    mu_assert(message, exact_mismatches(c, &cpu_first) == 0u);
    return NULL;
}

static char *test_cambi_metal_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(TWIN_NAME);
    mu_assert("integer_cambi_metal extractor must be registered", fex != NULL);
    mu_assert("integer_cambi_metal name matches", !strcmp(fex->name, TWIN_NAME));
    return NULL;
}

/* The gradient: 32-code steps are not banding to CAMBI, and score 0. */
static char *test_cambi_gradient_exact(void)
{
    static const CambiCase c = {"gradient", 256u, 256u, 8u, false, NULL, NULL, DEFAULT_KEY};
    return check_case(&c, "cambi CPU vs. Metal score is not bit-exact on the gradient");
}

/* The textured fixture reaches the stages the gradient leaves unobservable: a
 * twin that clamped the out-of-image neighbours of the spatial mask, or
 * rewrote the filter's border rows, drifted by 2.7e-3 on it
 * (T-GPU-CAMBI-PARITY-DRIFT-2026-09-05). Its +1 dither is a 4-code step at 10
 * bits, inside the contrast range, so the score must be non-zero. */
static char *test_cambi_textured_exact(void)
{
    static const CambiCase c = {"textured", 256u, 256u, 8u, true, NULL, NULL, DEFAULT_KEY};
    double cpu_first = 0.0;
    mu_assert("cambi CPU vs. Metal score is not bit-exact on the textured fixture",
              exact_mismatches(&c, &cpu_first) == 0u);
    mu_assert("the textured cambi fixture must score non-zero", mu_skipped || cpu_first > 0.0);
    return NULL;
}

static char *test_cambi_10bit_exact(void)
{
    static const CambiCase c = {"10-bit", 256u, 256u, 10u, true, NULL, NULL, DEFAULT_KEY};
    return check_case(&c, "cambi CPU vs. Metal score is not bit-exact at 10 bits");
}

/* A cap between the CPU's frame scores: the lowest and highest of the
 * unclipped textured scores must straddle it, so a twin that clips at the wrong
 * value, or not at all, scores a frame differently. `cap` receives the option
 * value (three significant digits) and `key` the score name it derives. */
static char *choose_cap(const CambiCase *base, char *cap, size_t cap_len, char *key, size_t key_len)
{
    double scores[NUM_FRAMES] = {0.0};
    mu_assert("CPU cambi run failed", !cambi_scores(NULL, base, scores));
    double lo = scores[0];
    double hi = scores[0];
    for (unsigned i = 1u; i < NUM_FRAMES; i++) {
        lo = scores[i] < lo ? scores[i] : lo;
        hi = scores[i] > hi ? scores[i] : hi;
    }
    mu_assert("cambi frame scores must differ to place a cap between them", hi > lo);
    (void)snprintf(cap, cap_len, "%.3g", (lo + hi) / 2.0);
    const double value = strtod(cap, NULL);
    mu_assert("a cap must lie between the lowest and highest frame score",
              value > lo && value < hi);
    (void)snprintf(key, key_len, "cambi_cmxv_%g", value);
    return NULL;
}

/* cambi_max_val clips the score at the cap. */
static char *test_cambi_max_val_exact(void)
{
    static const CambiCase base = {"cambi_max_val base", 256u, 256u, 8u, true, NULL, NULL,
                                   DEFAULT_KEY};
    char cap[32];
    char key[64];
    mu_assert_msg(choose_cap(&base, cap, sizeof(cap), key, sizeof(key)));
    const CambiCase c = {"cambi_max_val", 256u, 256u, 8u, true, "cambi_max_val", cap, key};
    return check_case(&c, "cambi CPU vs. Metal differs with cambi_max_val");
}

static char *test_cambi_vis_lum_threshold_exact(void)
{
    static const CambiCase c = {"cambi_vis_lum_threshold", 256u, 256u,          8u, true,
                                "cambi_vis_lum_threshold", "30", "cambi_vlt_30"};
    return check_case(&c, "cambi CPU vs. Metal differs with cambi_vis_lum_threshold");
}

static char *test_cambi_topk_exact(void)
{
    static const CambiCase c = {"cambi_topk", 256u,         256u,  8u,
                                true,         "cambi_topk", "0.2", "cambi_ctpk_0.2"};
    return check_case(&c, "cambi CPU vs. Metal differs with cambi_topk");
}

static char *test_cambi_window_size_exact(void)
{
    static const CambiCase c = {"window_size", 256u,          256u, 8u,
                                true,          "window_size", "31", "cambi_ws_31"};
    return check_case(&c, "cambi CPU vs. Metal differs with window_size");
}

static char *test_cambi_max_log_contrast_exact(void)
{
    static const CambiCase c = {"max_log_contrast", 256u, 256u,         10u, true,
                                "max_log_contrast", "4",  "cambi_mlc_4"};
    return check_case(&c, "cambi CPU vs. Metal differs with max_log_contrast");
}

/* At 1080p the speed-up downsamples after the spatial mask; the frame is
 * large enough for the option to act. */
static char *test_cambi_high_res_speedup_exact(void)
{
    static const CambiCase c = {"cambi_high_res_speedup", 1920u,  1080u,           8u, true,
                                "cambi_high_res_speedup", "1080", "cambi_hrs_1080"};
    return check_case(&c, "cambi CPU vs. Metal differs with cambi_high_res_speedup at 1080p");
}

static void run_default_cases(void)
{
    metal_run_case(test_cambi_gradient_exact);
    metal_run_case(test_cambi_textured_exact);
    metal_run_case(test_cambi_10bit_exact);
}

static void run_option_cases(void)
{
    metal_run_case(test_cambi_max_val_exact);
    metal_run_case(test_cambi_vis_lum_threshold_exact);
    metal_run_case(test_cambi_topk_exact);
    metal_run_case(test_cambi_window_size_exact);
    metal_run_case(test_cambi_max_log_contrast_exact);
    metal_run_case(test_cambi_high_res_speedup_exact);
}

char *run_tests(void)
{
    metal_run_case(test_cambi_metal_registered);
    run_default_cases();
    run_option_cases();
    return metal_first_failure;
}

/* NOLINTEND(modernize-use-nullptr) */
