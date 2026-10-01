/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  speed_chroma_cuda / speed_temporal_cuda against the CPU extractors with
 *  speed_prescale_method=lanczos4 (T-GPU-SPEED-LANCZOS4-PRESCALE-DRIFT-2026-09-30).
 *
 *  The CPU scaler evaluates each lanczos4 weight in fp64 with sin() and rounds
 *  it once. The scale kernel used to evaluate the weights itself in fp32 with
 *  sinpif(), a few ulp off on some of them, and SpEED amplifies that on smooth
 *  content. The kernel now reads the weights the host evaluates once per run
 *  with the scaler's own routine (speed_internal_gpu_lanczos_weights()).
 *
 *  Fixture: a smooth moving field of bilinear patches with a noisy,
 *  lower-contrast distorted side, 3 frames; 1920x1080 for the downscale and
 *  640x360 for the upscale, so the CPU scaler stays cheap.
 *    positive  prescale 0.5 and 2.0, both twins: every output of every frame
 *              is within LZ_TOLERANCE of the CPU;
 *    boundary  prescale 1.0 with lanczos4 named: nothing is resampled and no
 *              weight table exists; the twin still matches.
 *  The negative cases of the table (bad arguments, no table for the other
 *  methods) need no device: test_speed_lanczos4_weights.
 *
 *  Measured on an RTX 4090 (glibc 2.44). With the fp32 weights the worst
 *  relative difference on this fixture was 8.8e-3 for speed_chroma at 0.5
 *  (0.27 absolute, on scores near 30), 8.2e-5 for speed_chroma at 2.0, and
 *  5.6e-7 for speed_temporal. With the host table every output of every frame
 *  is bit-identical. LZ_TOLERANCE is 1e-6 relative rather than zero because
 *  the twins' one host-dependent operation is the CPU's log2f(), which glibc
 *  misrounds for about 0.4% of arguments (ADR-1380): a different libm may move
 *  a score by an ulp, 1.2e-7 relative at most.
 *
 *  Skips (exit 77) when no CUDA device is visible.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr) -- ADR-1138: retain NULL for Windows C
 * support and upstream-compatible C test conventions. */

/* Downscaling halves a 1920x1080 frame, upscaling doubles a 640x360 one, so
 * both leave the chroma planes with enough 5x5 blocks to score. */
#define LZ_DOWN_W 1920u
#define LZ_DOWN_H 1080u
#define LZ_UP_W 640u
#define LZ_UP_H 360u
#define LZ_FRAMES 3u
#define LZ_MAX_KEYS 3u
#define LZ_TOLERANCE 1e-6

typedef struct LzRun {
    const char *cpu;      /* CPU extractor */
    const char *cuda;     /* CUDA twin */
    const char *prescale; /* speed_prescale */
    unsigned w;           /* luma width */
    unsigned h;           /* luma height */
    const char *keys[LZ_MAX_KEYS];
} LzRun;

typedef struct LzScores {
    double v[LZ_FRAMES][LZ_MAX_KEYS];
} LzScores;

/* A triangle wave of `period` samples, 0 .. period / 2. */
static unsigned lz_triangle(unsigned v, unsigned period)
{
    const unsigned phase = v % period;
    return phase < period / 2u ? phase : period - phase;
}

/* One sample of a smooth field, the product of a horizontal and a vertical
 * triangle wave (bilinear patches, so every neighbourhood is a gradient),
 * moving three columns a frame. The distorted side loses a quarter of the
 * contrast and gains 0..7 of deterministic noise. */
static uint8_t lz_pixel(const VmafPicture *pic, unsigned plane, unsigned row, unsigned col,
                        unsigned frame, uint32_t *noise)
{
    /* Five patches across and four down, whatever the plane size, so the
     * structure survives SpEED's 16x decimation at every prescale used here. */
    const unsigned px = 2u * (pic->w[plane] / 10u);
    const unsigned py = 2u * (pic->h[plane] / 8u);
    const unsigned field = lz_triangle(col + 3u * frame, px) * lz_triangle(row, py);
    unsigned value = 40u + 600u * field / (px * py); /* 40 .. 190 */
    if (noise) {
        *noise = *noise * 1664525u + 1013904223u;
        value = 20u + 3u * value / 4u + (*noise >> 29u);
    }
    return (uint8_t)value;
}

static int lz_fill(VmafPicture *pic, const LzRun *run, unsigned frame, bool distort)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8u, run->w, run->h);
    if (err)
        return err;
    uint32_t noise = 0x9e3779b9u + frame;
    for (unsigned p = 0; p < 3u; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                plane[(ptrdiff_t)row * pic->stride[p] + col] =
                    lz_pixel(pic, p, row, col, frame, distort ? &noise : NULL);
            }
        }
    }
    return 0;
}

static int lz_use(VmafContext *vmaf, const char *extractor, const char *prescale)
{
    VmafFeatureDictionary *opts = NULL;
    int err = vmaf_feature_dictionary_set(&opts, "speed_prescale", prescale);
    if (!err)
        err = vmaf_feature_dictionary_set(&opts, "speed_prescale_method", "lanczos4");
    if (!err)
        return vmaf_use_feature(vmaf, extractor, opts);
    (void)vmaf_feature_dictionary_free(&opts);
    return err;
}

static int lz_feed(VmafContext *vmaf, const LzRun *run)
{
    for (unsigned i = 0; i < LZ_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = lz_fill(&ref, run, i, false);
        if (err)
            return err;
        err = lz_fill(&dist, run, i, true);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            return err;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err)
            return err;
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

static int lz_collect(VmafContext *vmaf, const LzRun *run, LzScores *out)
{
    for (unsigned k = 0; k < LZ_MAX_KEYS && run->keys[k]; k++) {
        for (unsigned i = 0; i < LZ_FRAMES; i++) {
            const int err = vmaf_feature_score_at_index(vmaf, run->keys[k], &out->v[i][k], i);
            if (err)
                return err;
        }
    }
    return 0;
}

/* Name the stage that failed, so a red run says more than "failed". */
static int lz_stage(const char *stage, const char *extractor, int err)
{
    if (err)
        (void)fprintf(stderr, "\n%s: %s failed (%d)\n", extractor, stage, err);
    return err;
}

/* Score every frame with one extractor; `cu_state` NULL runs the CPU one. */
static int lz_score(const LzRun *run, VmafCudaState *cu_state, LzScores *out)
{
    const char *name = cu_state ? run->cuda : run->cpu;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = lz_stage("vmaf_init", name, vmaf_init(&vmaf, cfg));
    if (err)
        return err;
    if (cu_state)
        err = lz_stage("vmaf_cuda_import_state", name, vmaf_cuda_import_state(vmaf, cu_state));
    if (!err)
        err = lz_stage("vmaf_use_feature", name, lz_use(vmaf, name, run->prescale));
    if (!err)
        err = lz_stage("vmaf_read_pictures", name, lz_feed(vmaf, run));
    if (!err)
        err = lz_stage("vmaf_feature_score_at_index", name, lz_collect(vmaf, run, out));
    const int closed = lz_stage("vmaf_close", name, vmaf_close(vmaf));
    return err ? err : closed;
}

/* The twin's scores, on a CUDA state of its own (a state serves one context).
 * Returns 1 when no device is visible, which is a skip and not a failure. */
static int lz_score_cuda(const LzRun *run, LzScores *out)
{
    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    if (vmaf_cuda_state_init(&cu_state, cuda_cfg) != 0 || !cu_state)
        return 1;
    const int err = lz_score(run, cu_state, out);
    const int freed = lz_stage("vmaf_cuda_state_free", run->cuda, vmaf_cuda_state_free(cu_state));
    return err ? err : freed;
}

/* The worst relative difference over every frame and output of one run. */
static double lz_worst(const LzRun *run, const LzScores *cpu, const LzScores *gpu)
{
    double worst = 0.0;
    for (unsigned k = 0; k < LZ_MAX_KEYS && run->keys[k]; k++) {
        for (unsigned i = 0; i < LZ_FRAMES; i++) {
            const double want = cpu->v[i][k];
            const double delta = fabs(want - gpu->v[i][k]);
            const double relative = want != 0.0 ? delta / fabs(want) : delta;
            if (!(relative <= worst)) /* also catches NaN */
                worst = relative;
        }
    }
    return worst;
}

static char *lz_check(const LzRun *run)
{
    LzScores cpu;
    LzScores gpu;
    memset(&cpu, 0, sizeof(cpu));
    memset(&gpu, 0, sizeof(gpu));
    const int device = lz_score_cuda(run, &gpu);
    if (device == 1) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        mu_skipped = 1; /* exit 77, not a pass */
        return NULL;
    }
    mu_assert("CUDA twin failed", device == 0);
    mu_assert("CPU extractor failed", lz_score(run, NULL, &cpu) == 0);
    mu_assert("the fixture scores nothing", cpu.v[1][0] > 0.0);

    const double worst = lz_worst(run, &cpu, &gpu);
    if (!(worst <= LZ_TOLERANCE)) {
        (void)fprintf(stderr, "\n%s prescale %s lanczos4: worst relative difference %.3e > %.1e\n",
                      run->cuda, run->prescale, worst, LZ_TOLERANCE);
    }
    mu_assert("lanczos4 prescale: the CUDA twin differs from the CPU extractor",
              worst <= LZ_TOLERANCE);
    return NULL;
}

static char *test_lanczos4_prescale_matches_cpu(void)
{
    static const LzRun runs[] = {
        {"speed_chroma",
         "speed_chroma_cuda",
         "0.5",
         LZ_DOWN_W,
         LZ_DOWN_H,
         {"speed_chroma_u_ps_0.5_psm_lanczos4", "speed_chroma_v_ps_0.5_psm_lanczos4",
          "speed_chroma_uv_ps_0.5_psm_lanczos4"}},
        {"speed_temporal",
         "speed_temporal_cuda",
         "0.5",
         LZ_DOWN_W,
         LZ_DOWN_H,
         {"speed_temporal_ps_0.5_psm_lanczos4", NULL, NULL}},
        {"speed_chroma",
         "speed_chroma_cuda",
         "2",
         LZ_UP_W,
         LZ_UP_H,
         {"speed_chroma_u_ps_2_psm_lanczos4", "speed_chroma_v_ps_2_psm_lanczos4",
          "speed_chroma_uv_ps_2_psm_lanczos4"}},
        {"speed_temporal",
         "speed_temporal_cuda",
         "2",
         LZ_UP_W,
         LZ_UP_H,
         {"speed_temporal_ps_2_psm_lanczos4", NULL, NULL}},
        {"speed_temporal",
         "speed_temporal_cuda",
         "1",
         LZ_UP_W,
         LZ_UP_H,
         {"speed_temporal_psm_lanczos4", NULL, NULL}},
    };
    for (size_t i = 0; !mu_skipped && i < sizeof(runs) / sizeof(runs[0]); i++)
        mu_assert_msg(lz_check(&runs[i]));
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_lanczos4_prescale_matches_cpu);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
