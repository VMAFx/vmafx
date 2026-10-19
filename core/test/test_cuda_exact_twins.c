/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The CUDA twins that ADR-1457 declares exact return the CPU extractor's
 * bits.
 *
 * `motion_cuda`, `motion_v2_cuda`, `psnr_cuda`, `float_ssim_cuda`,
 * `float_ms_ssim_cuda` and `cambi_cuda` are listed as exact twins of their
 * CPU extractors: the parity gate compares them with tolerance 0. Each
 * reaches the CPU's value by construction. The motion and PSNR kernels
 * accumulate integers and the host concludes through the CPU's own helpers
 * (ADR-1372, ADR-1373); the SSIM and MS-SSIM kernels run the CPU's arithmetic
 * type for type (ADR-1399, ADR-1403); CAMBI is integer up to a fixed-point
 * top-K sum and a host combine that is the CPU's (ADR-1379).
 *
 * This test holds every one of them to that: four frames at 8 and at 10 bits,
 * `==` on every output, the debug, L / C / S and per-scale outputs included.
 * The frames move, so the motion scores are not zero, and half of each frame
 * is a ramp of single code levels, so CAMBI scores above zero. The frame is
 * 640x480, so float_ssim decimates by two before it scores. Their own parity
 * tests keep their fixtures for coverage of options and sizes; a twin that
 * drifts by a last bit fails here.
 *
 * One CAMBI case gives the CPU extractor full_ref with a source twice the
 * picture, options the twin does not declare: the distorted score must stay
 * the twin's (T-CAMBI-10BIT-FULLREF-WIDE-SOURCE-ROWS-2026-10-05).
 *
 * Skip behaviour: without a CUDA device a case reports the skip and the run
 * exits 77.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

/* Both dimensions clear CAMBI's minimum (216) and MS-SSIM's (176); the
 * short side makes float_ssim's automatic scale 2. */
#define FIXTURE_W 640u
#define FIXTURE_H 480u

#define MAX_KEYS 16u
/* Key and value strings of a case's CPU-only options, NULL-terminated. */
#define MAX_CPU_OPTS 7u
/* Frames per run: motion needs three for its first blended score, and a
 * fourth shows a steady state. */
#define NUM_FRAMES 4u

/* One exact twin with one option set. */
typedef struct ExactCase {
    const char *cuda;    /* CUDA extractor */
    const char *cpu;     /* its CPU twin */
    const char *opt_key; /* option both get, or NULL */
    const char *opt_val;
    bool nonzero;               /* the first key must not be 0 on every frame */
    const char *keys[MAX_KEYS]; /* outputs compared, NULL-terminated */
    /* Options only the CPU extractor gets (key, value, ...): a configuration
     * the twin does not declare, which must leave the CPU's scores of `keys`
     * unchanged. The CPU then stores them under `cpu_keys`. */
    const char *cpu_opts[MAX_CPU_OPTS];
    const char *cpu_keys[MAX_KEYS];
} ExactCase;

#define MS_SSIM_LCS_KEYS(term)                                                                     \
    "float_ms_ssim_" term "_scale0", "float_ms_ssim_" term "_scale1",                              \
        "float_ms_ssim_" term "_scale2", "float_ms_ssim_" term "_scale3",                          \
        "float_ms_ssim_" term "_scale4"

#define SSIM_LCS_KEYS "float_ssim_l", "float_ssim_c", "float_ssim_s"

static const ExactCase cases[] = {
    {"motion_cuda",
     "motion",
     NULL,
     NULL,
     true,
     {"VMAF_integer_feature_motion2_score", "VMAF_integer_feature_motion3_score"},
     .cpu_opts = {NULL}},
    {"motion_cuda",
     "motion",
     "debug",
     "true",
     true,
     {"VMAF_integer_feature_motion_score", "VMAF_integer_feature_motion2_score",
      "VMAF_integer_feature_motion3_score"},
     .cpu_opts = {NULL}},
    {"motion_v2_cuda",
     "motion_v2",
     NULL,
     NULL,
     true,
     {"VMAF_integer_feature_motion_v2_sad_score", "VMAF_integer_feature_motion2_v2_score",
      "VMAF_integer_feature_motion3_v2_score"},
     .cpu_opts = {NULL}},
    {"psnr_cuda", "psnr", NULL, NULL, true, {"psnr_y", "psnr_cb", "psnr_cr"}, .cpu_opts = {NULL}},
    {"float_ssim_cuda", "float_ssim", NULL, NULL, true, {"float_ssim"}, .cpu_opts = {NULL}},
    {"float_ssim_cuda",
     "float_ssim",
     "enable_lcs",
     "true",
     true,
     {"float_ssim", SSIM_LCS_KEYS},
     .cpu_opts = {NULL}},
    {"float_ms_ssim_cuda",
     "float_ms_ssim",
     NULL,
     NULL,
     true,
     {"float_ms_ssim"},
     .cpu_opts = {NULL}},
    {"float_ms_ssim_cuda",
     "float_ms_ssim",
     "enable_lcs",
     "true",
     true,
     {"float_ms_ssim", MS_SSIM_LCS_KEYS("l"), MS_SSIM_LCS_KEYS("c"), MS_SSIM_LCS_KEYS("s")},
     .cpu_opts = {NULL}},
    /* enable_chroma: the 320x240 chroma of the fixture clears the 176-pixel
     * minimum and carries its own texture and error. */
    {"float_ms_ssim_cuda",
     "float_ms_ssim",
     "enable_chroma",
     "true",
     true,
     {"float_ms_ssim", "float_ms_ssim_cb", "float_ms_ssim_cr"},
     .cpu_opts = {NULL}},
    {"cambi_cuda", "cambi", NULL, NULL, true, {"Cambi_feature_cambi_score"}, .cpu_opts = {NULL}},
    /* full_ref with a source twice the picture: `cambi` stays the distorted
     * picture's score at the encode size, the score the twin computes
     * (T-CAMBI-10BIT-FULLREF-WIDE-SOURCE-ROWS-2026-10-05). */
    {.cuda = "cambi_cuda",
     .cpu = "cambi",
     .nonzero = true,
     .keys = {"Cambi_feature_cambi_score"},
     .cpu_opts = {"full_ref", "true", "src_width", "1280", "src_height", "960", NULL},
     .cpu_keys = {"cambi_srch_960_srcw_1280"}},
};
#define N_CASES (sizeof(cases) / sizeof(cases[0]))

static size_t key_count(const ExactCase *c)
{
    size_t n = 0u;
    while (n < MAX_KEYS && c->keys[n] != NULL) {
        n++;
    }
    return n;
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

/* Luma of frame `frame`. Left half: a shallow ramp that shifts with the
 * frame, one 10-bit code level every two columns (one 8-bit level every
 * eight), which is banding for CAMBI at both depths. Right half: texture that
 * moves down three rows a frame; the distorted frame adds a
 * position-dependent error there and one 10-bit level on the ramp. */
static unsigned luma(unsigned row, unsigned col, unsigned frame, bool distorted, unsigned bpc)
{
    if (col < FIXTURE_W / 2u) {
        const unsigned ramp = 200u + ((col + frame) / 2u) + (distorted ? 1u : 0u);
        return ramp >> (10u - bpc);
    }
    row += frame * 3u;
    unsigned v = (((row * 3u) + (col * 2u)) & 0xFFu) ^ (((row >> 2) * (col >> 3)) & 0x1Fu);
    if (distorted) {
        v += 9u + (((row * 5u) + (col * 7u)) % 11u);
    }
    return v << (bpc - 8u);
}

static unsigned chroma(unsigned plane, unsigned row, unsigned col, bool distorted, unsigned gain)
{
    const unsigned v = 96u + (((row * 3u) + (col * 5u) + (plane * 17u)) & 0x3Fu);
    return (v + (distorted ? ((row + col) % 5u) : 0u)) * gain;
}

static int fill_picture(VmafPicture *pic, unsigned bpc, unsigned frame, bool distorted)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, bpc, FIXTURE_W, FIXTURE_H);
    if (err) {
        return err;
    }
    const unsigned gain = 1u << (bpc - 8u);
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            put_sample(pic, 0u, row, col, luma(row, col, frame, distorted, bpc));
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                put_sample(pic, p, row, col, chroma(p, row, col, distorted, gain));
            }
        }
    }
    return 0;
}

/* Frame `frame` of the fixture through `vmaf`, which takes both pictures. */
static int feed_frame(VmafContext *vmaf, unsigned bpc, unsigned frame)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_picture(&ref, bpc, frame, false);
    if (err) {
        return err;
    }
    err = fill_picture(&dist, bpc, frame, true);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, frame);
}

/* The case's CPU-only options, added to `opts`. */
static int add_cpu_options(VmafFeatureDictionary **opts, const ExactCase *c)
{
    int err = 0;
    for (size_t i = 0; i + 1u < MAX_CPU_OPTS && c->cpu_opts[i] != NULL && !err; i += 2u) {
        err = vmaf_feature_dictionary_set(opts, c->cpu_opts[i], c->cpu_opts[i + 1u]);
    }
    return err;
}

/* The keys the case's outputs are stored under on the device or the CPU. */
static const char *const *case_keys(const ExactCase *c, bool on_device)
{
    return (!on_device && c->cpu_keys[0] != NULL) ? c->cpu_keys : c->keys;
}

/* A context with the case's CPU extractor, or its twin on `cu_state`. */
static int case_context(VmafContext **vmaf, const ExactCase *c, VmafCudaState *cu_state)
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafFeatureDictionary *opts = NULL;
    int err = vmaf_init(vmaf, cfg);
    if (!err && cu_state) {
        err = vmaf_cuda_import_state(*vmaf, cu_state);
    }
    if (!err && c->opt_key) {
        err = vmaf_feature_dictionary_set(&opts, c->opt_key, c->opt_val);
    }
    if (!err && !cu_state) {
        err = add_cpu_options(&opts, c);
    }
    if (!err) {
        /* vmaf_use_feature() takes the dictionary over, on failure too. */
        err = vmaf_use_feature(*vmaf, cu_state ? c->cuda : c->cpu, opts);
    }
    return err;
}

/* NUM_FRAMES frames through one extractor, and every key of the case of every
 * frame read into `out` (frame-major). Returns the first error. */
static int case_scores(const ExactCase *c, VmafCudaState *cu_state, unsigned bpc, double *out)
{
    const size_t count = key_count(c);
    const char *const *keys = case_keys(c, cu_state != NULL);
    VmafContext *vmaf = NULL;
    int err = case_context(&vmaf, c, cu_state);
    for (unsigned frame = 0; frame < NUM_FRAMES && !err; frame++) {
        err = feed_frame(vmaf, bpc, frame);
    }
    if (!err) {
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    }
    for (size_t i = 0; i < count * NUM_FRAMES && !err; i++) {
        err = vmaf_feature_score_at_index(vmaf, keys[i % count], &out[i], (unsigned)(i / count));
        if (err) {
            (void)fprintf(stderr, "\n%s: no score for %s at frame %u\n",
                          cu_state ? c->cuda : c->cpu, keys[i % count], (unsigned)(i / count));
        }
    }
    const int closed = vmaf ? vmaf_close(vmaf) : 0;
    return err ? err : closed;
}

/* The device, or NULL with the reason printed when there is none. */
static VmafCudaState *cuda_device(void)
{
    VmafCudaState *cu_state = NULL;
    const VmafCudaConfiguration cuda_cfg = {0};
    if (vmaf_cuda_state_init(&cu_state, cuda_cfg) != 0 || cu_state == NULL) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        mu_skipped = 1;
        return NULL;
    }
    return cu_state;
}

/* The outputs whose CUDA value is not the CPU's, each one reported, plus one
 * when the case's first key is 0 on every frame (a fixture that measures
 * nothing). */
static unsigned count_mismatches(const ExactCase *c, unsigned bpc, const double *cpu,
                                 const double *gpu)
{
    const size_t count = key_count(c);
    unsigned mismatches = 0u;
    bool any_nonzero = false;
    for (size_t i = 0; i < count * NUM_FRAMES; i++) {
        any_nonzero = any_nonzero || ((i % count) == 0u && cpu[i] != 0.0);
        if (isfinite(cpu[i]) && cpu[i] == gpu[i]) {
            continue;
        }
        mismatches++;
        (void)fprintf(stderr, "\n%s %u-bit frame %u %s: cpu=%.17g cuda=%.17g delta=%.3e\n", c->cuda,
                      bpc, (unsigned)(i / count), c->keys[i % count], cpu[i], gpu[i],
                      fabs(cpu[i] - gpu[i]));
    }
    if (c->nonzero && !any_nonzero) {
        (void)fprintf(stderr, "\n%s %u-bit: %s is 0 on every frame\n", c->cuda, bpc, c->keys[0]);
        mismatches++;
    }
    return mismatches;
}

/* Mismatches of one case at one bit depth; UINT32_MAX when a run failed. A
 * skipped CUDA leg counts as 0. */
static unsigned exact_mismatches(const ExactCase *c, unsigned bpc)
{
    double cpu[MAX_KEYS * NUM_FRAMES] = {0.0};
    double gpu[MAX_KEYS * NUM_FRAMES] = {0.0};
    VmafCudaState *cu_state = cuda_device();
    if (!cu_state) {
        return 0u;
    }
    const int gpu_err = case_scores(c, cu_state, bpc, gpu);
    const int free_err = vmaf_cuda_state_free(cu_state);
    const int cpu_err = gpu_err ? 0 : case_scores(c, NULL, bpc, cpu);
    if (gpu_err || cpu_err || free_err) {
        (void)fprintf(stderr, "\n%s %u-bit: run failed (cuda %d, cpu %d, free %d)\n", c->cuda, bpc,
                      gpu_err, cpu_err, free_err);
        return UINT32_MAX;
    }
    return count_mismatches(c, bpc, cpu, gpu);
}

/* Every case at one bit depth; all of them run, so one failure does not hide
 * the next twin's. */
static unsigned mismatches_at(unsigned bpc)
{
    unsigned failed_cases = 0u;
    for (size_t i = 0; i < N_CASES && !mu_skipped; i++) {
        failed_cases += (exact_mismatches(&cases[i], bpc) != 0u) ? 1u : 0u;
    }
    return failed_cases;
}

static char *test_exact_twins_8bit(void)
{
    mu_assert("a CUDA twin declared exact is not bit-identical to its CPU extractor at 8 bits",
              mismatches_at(8u) == 0u);
    return NULL;
}

static char *test_exact_twins_10bit(void)
{
    mu_assert("a CUDA twin declared exact is not bit-identical to its CPU extractor at 10 bits",
              mismatches_at(10u) == 0u);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_exact_twins_8bit);
    mu_run_test(test_exact_twins_10bit);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
