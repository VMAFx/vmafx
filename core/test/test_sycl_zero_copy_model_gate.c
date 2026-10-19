/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The SYCL zero-copy path scores what it can and names what it cannot
 * (ADR-1688), on a SYCL device.
 *
 * vmaf_read_pictures_sycl() reads the luma plane the caller put into the
 * shared frame (here with vmaf_sycl_upload_plane(), the host form of the
 * DMA-BUF / VA import) and hands the extractors no host picture. Before
 * ADR-1688, on an Arc A380 with FFmpeg's libvmaf_sycl filter:
 * - the default model vmaf_v1.0.16_3d0h failed on the first frame with a
 *   bare -22 (speed_chroma_sycl needs the U and V planes);
 * - float_psnr_sycl dereferenced the NULL picture and crashed;
 * - motion_sycl with motion_add_uv added the SAD of chroma it never imported;
 * - a CPU extractor (float_psnr) was skipped on every frame without an error.
 * Each now makes the call return -ENOTSUP before the frame is counted, and
 * the context flushes cleanly afterwards. vmaf_v0.6.1, whose features all
 * read luma, still runs and gives the CPU's per-frame scores, and so does
 * every other twin the path admits (psnr / psnr_hvs without chroma,
 * float_moment, motion_v2, cambi).
 *
 * Skip behaviour: without a SYCL device the test prints
 * "[skip: no SYCL device]" and exits 77.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/model.h"
#include "libvmaf/picture.h"
#include "sycl/common.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit; MSVC's C mode has
 * no `nullptr`. ADR-1138. */

#define GATE_W 576u
#define GATE_H 324u
#define GATE_FRAMES 3u
#define MAX_FRAMES 8u

/* The geometry and length of the runs; the upload-ordering case widens them. */
static unsigned g_w = GATE_W;
static unsigned g_h = GATE_H;
static unsigned g_frames = GATE_FRAMES;

static uint8_t luma(unsigned row, unsigned col, unsigned frame, unsigned salt)
{
    const unsigned mix = (row * 7u + col * 3u + frame * 11u) ^ ((row * col + salt) * 5u);
    return (uint8_t)((mix + salt * 17u) & 0xFFu);
}

static int fill_frame(VmafPicture *pic, unsigned frame, unsigned salt)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8u, g_w, g_h);
    if (err) {
        return err;
    }
    for (unsigned p = 0u; p < 3u; p++) {
        uint8_t *data = (uint8_t *)pic->data[p];
        for (unsigned row = 0u; row < pic->h[p]; row++) {
            for (unsigned col = 0u; col < pic->w[p]; col++) {
                data[(size_t)row * (size_t)pic->stride[p] + col] = luma(row, col, frame, salt + p);
            }
        }
    }
    return 0;
}

typedef struct Gate {
    VmafSyclState *state;
    VmafContext *vmaf;
    VmafModel *model;
} Gate;

static int open_state(VmafSyclState **state)
{
    const VmafSyclConfiguration cfg = {.device_index = -1};
    if (vmaf_sycl_state_init(state, cfg) != 0 || *state == NULL) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        mu_skipped = 1;
        return -ENODEV;
    }
    return 0;
}

/* A zero-copy context on `state` with `model` (or none) and `feature` (or none,
 * with option key=val when key is set). */
static int open_gate(Gate *g, VmafSyclState *state, const char *model, const char *feature,
                     const char *key, const char *val)
{
    memset(g, 0, sizeof(*g));
    g->state = state;
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_ERROR};
    int err = vmaf_init(&g->vmaf, cfg);
    err = err ? err : vmaf_sycl_import_state(g->vmaf, state);
    err = err ? err : vmaf_sycl_init_frame_buffers(g->vmaf, g_w, g_h, 8u);
    if (!err && model) {
        VmafModelConfig mcfg = {.name = "vmaf", .flags = VMAF_MODEL_FLAGS_DEFAULT};
        err = vmaf_model_load(&g->model, &mcfg, model);
        err = err ? err : vmaf_use_features_from_model(g->vmaf, g->model);
    }
    if (!err && feature) {
        VmafFeatureDictionary *opts = NULL;
        if (key) {
            err = vmaf_feature_dictionary_set(&opts, key, val);
        }
        err = err ? err : vmaf_use_feature(g->vmaf, feature, opts);
    }
    return err;
}

static int close_gate(Gate *g)
{
    const int err = g->vmaf ? vmaf_close(g->vmaf) : 0;
    if (g->model) {
        vmaf_model_destroy(g->model);
    }
    return err;
}

/* Put frame `frame`'s luma into the shared frame and read it. */
static int read_zero_copy(Gate *g, unsigned frame)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_frame(&ref, frame, 0u);
    err = err ? err : fill_frame(&dist, frame, 9u);
    if (err) {
        return err;
    }
    /* No wait of our own: vmaf_read_pictures_sycl() must order the compute
     * after the uploads (T-SYCL-UPLOAD-PLANE-NO-COMPUTE-FENCE-2026-10-05). */
    err = vmaf_sycl_upload_plane(g->state, ref.data[0], (unsigned)ref.stride[0], 1, g_w, g_h, 8u);
    err = err ? err :
                vmaf_sycl_upload_plane(g->state, dist.data[0], (unsigned)dist.stride[0], 0, g_w,
                                       g_h, 8u);
    err = err ? err : vmaf_read_pictures_sycl(g->vmaf, frame);
    const int unref_ref = vmaf_picture_unref(&ref);
    const int unref_dist = vmaf_picture_unref(&dist);
    if (err) {
        return err;
    }
    return unref_ref ? unref_ref : unref_dist;
}

/* The first frame is refused with -ENOTSUP, again on a retry, and the
 * context still flushes and closes. */
static char *refused(VmafSyclState *state, const char *model, const char *feature, const char *key,
                     const char *val)
{
    Gate g;
    mu_assert("zero-copy context opens", open_gate(&g, state, model, feature, key, val) == 0);
    mu_assert("first frame refused with -ENOTSUP", read_zero_copy(&g, 0u) == -ENOTSUP);
    mu_assert("a retry is refused the same way", read_zero_copy(&g, 0u) == -ENOTSUP);
    mu_assert("the refused context flushes without error", vmaf_flush_sycl(g.vmaf) == 0);
    mu_assert("the refused context closes", close_gate(&g) == 0);
    return NULL;
}

static char *test_default_model_refused(void)
{
    VmafSyclState *state = NULL;
    if (open_state(&state)) {
        return NULL;
    }
    char *msg = refused(state, "vmaf_v1.0.16_3d0h", NULL, NULL, NULL);
    vmaf_sycl_state_free(&state);
    return msg;
}

static char *test_host_picture_features_refused(void)
{
    VmafSyclState *state = NULL;
    if (open_state(&state)) {
        return NULL;
    }
    char *msg = refused(state, NULL, "float_psnr_sycl", NULL, NULL);
    msg = msg ? msg : refused(state, NULL, "motion_sycl", "motion_add_uv", "true");
    msg = msg ? msg : refused(state, NULL, "float_psnr", NULL, NULL);
    vmaf_sycl_state_free(&state);
    return msg;
}

/* What a context scores: a model (its VMAF), or one feature extractor with
 * an optional option and the collector key read back. */
typedef struct Spec {
    const char *model;
    const char *feature, *key, *val;
    const char *score;
} Spec;

static int open_plain(VmafContext **vmaf, VmafModel **model, const Spec *spec)
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_ERROR};
    VmafModelConfig mcfg = {.name = "vmaf", .flags = VMAF_MODEL_FLAGS_DEFAULT};
    int err = vmaf_init(vmaf, cfg);
    if (!err && spec->model) {
        err = vmaf_model_load(model, &mcfg, spec->model);
        err = err ? err : vmaf_use_features_from_model(*vmaf, *model);
    }
    if (!err && spec->feature) {
        VmafFeatureDictionary *opts = NULL;
        if (spec->key) {
            err = vmaf_feature_dictionary_set(&opts, spec->key, spec->val);
        }
        err = err ? err : vmaf_use_feature(*vmaf, spec->feature, opts);
    }
    return err;
}

static int read_scores(VmafContext *vmaf, VmafModel *model, const Spec *spec, double *out)
{
    int err = 0;
    for (unsigned f = 0u; !err && f < g_frames; f++) {
        err = model ? vmaf_score_at_index(vmaf, model, &out[f], f) :
                      vmaf_feature_score_at_index(vmaf, spec->score, &out[f], f);
    }
    return err;
}

/* `spec` from host frames, with the CPU extractors. */
static int cpu_scores(const Spec *spec, double *out)
{
    VmafContext *vmaf = NULL;
    VmafModel *model = NULL;
    int err = open_plain(&vmaf, &model, spec);
    for (unsigned f = 0u; !err && f < g_frames; f++) {
        VmafPicture ref;
        VmafPicture dist;
        err = fill_frame(&ref, f, 0u);
        err = err ? err : fill_frame(&dist, f, 9u);
        err = err ? err : vmaf_read_pictures(vmaf, &ref, &dist, f);
    }
    err = err ? err : vmaf_read_pictures(vmaf, NULL, NULL, 0);
    err = err ? err : read_scores(vmaf, model, spec, out);
    if (model) {
        vmaf_model_destroy(model);
    }
    const int close_err = vmaf ? vmaf_close(vmaf) : 0;
    return err ? err : close_err;
}

/* `spec` (with SYCL extractor names) on the zero-copy path. */
static int zero_copy_scores(VmafSyclState *state, const Spec *spec, double *out)
{
    Gate g;
    int err = open_gate(&g, state, spec->model, spec->feature, spec->key, spec->val);
    for (unsigned f = 0u; !err && f < g_frames; f++) {
        err = read_zero_copy(&g, f);
    }
    err = err ? err : vmaf_flush_sycl(g.vmaf);
    err = err ? err : read_scores(g.vmaf, g.model, spec, out);
    const int close_err = close_gate(&g);
    return err ? err : close_err;
}

/* The zero-copy scores of `zc` equal the CPU scores of `cpu`, frame by frame. */
static int same_as_cpu(VmafSyclState *state, const Spec *zc, const Spec *cpu)
{
    double a[MAX_FRAMES] = {0};
    double b[MAX_FRAMES] = {0};
    if (zero_copy_scores(state, zc, a) != 0 || cpu_scores(cpu, b) != 0) {
        (void)fprintf(stderr, "%s: a run failed\n", zc->model ? zc->model : zc->feature);
        return 0;
    }
    int same = 1;
    for (unsigned f = 0u; f < g_frames; f++) {
        (void)fprintf(stderr, "%s frame %u: cpu %.17g zero-copy %.17g\n",
                      zc->model ? zc->model : zc->feature, f, b[f], a[f]);
        same = same && (a[f] == b[f]);
    }
    return same;
}

static char *test_luma_model_runs_and_matches_cpu(void)
{
    VmafSyclState *state = NULL;
    if (open_state(&state)) {
        return NULL;
    }
    const Spec model = {.model = "vmaf_v0.6.1"};
    const int same = same_as_cpu(state, &model, &model);
    vmaf_sycl_state_free(&state);
    mu_assert("vmaf_v0.6.1 on the zero-copy path is the CPU's on every frame", same);
    return NULL;
}

/* Every other admitted twin runs on the zero-copy path and gives the CPU's
 * value: the hook's true is checked, not only its false. */
static char *test_admitted_twins_run_and_match_cpu(void)
{
    static const Spec cases[][2] = {
        {{.feature = "psnr_sycl", .key = "enable_chroma", .val = "false", .score = "psnr_y"},
         {.feature = "psnr", .key = "enable_chroma", .val = "false", .score = "psnr_y"}},
        {{.feature = "psnr_hvs_sycl", .key = "enable_chroma", .val = "false", .score = "psnr_hvs"},
         {.feature = "psnr_hvs", .key = "enable_chroma", .val = "false", .score = "psnr_hvs"}},
        {{.feature = "float_moment_sycl", .score = "float_moment_ref1st"},
         {.feature = "float_moment", .score = "float_moment_ref1st"}},
        {{.feature = "motion_v2_sycl", .score = "VMAF_integer_feature_motion_v2_sad_score"},
         {.feature = "motion_v2", .score = "VMAF_integer_feature_motion_v2_sad_score"}},
        {{.feature = "cambi_sycl", .score = "Cambi_feature_cambi_score"},
         {.feature = "cambi", .score = "Cambi_feature_cambi_score"}},
    };
    VmafSyclState *state = NULL;
    if (open_state(&state)) {
        return NULL;
    }
    int same = 1;
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        same = same_as_cpu(state, &cases[i][0], &cases[i][1]) && same;
    }
    vmaf_sycl_state_free(&state);
    mu_assert("an admitted twin on the zero-copy path is not the CPU's", same);
    return NULL;
}

/* vmaf_sycl_upload_plane() copies on the copy queue and the compute that
 * reads the shared frame runs on another queue. 3840x2160 frames that change
 * every frame, read right after the upload with no wait of the caller's own:
 * psnr_y equals the CPU's on every frame only if the compute is ordered after
 * the copy (T-SYCL-UPLOAD-PLANE-NO-COMPUTE-FENCE-2026-10-05). */
static char *test_upload_plane_orders_compute(void)
{
    VmafSyclState *state = NULL;
    if (open_state(&state)) {
        return NULL;
    }
    const Spec zc = {
        .feature = "psnr_sycl", .key = "enable_chroma", .val = "false", .score = "psnr_y"};
    const Spec cpu = {.feature = "psnr", .key = "enable_chroma", .val = "false", .score = "psnr_y"};
    g_w = 3840u;
    g_h = 2160u;
    g_frames = MAX_FRAMES;
    const int same = same_as_cpu(state, &zc, &cpu);
    g_w = GATE_W;
    g_h = GATE_H;
    g_frames = GATE_FRAMES;
    vmaf_sycl_state_free(&state);
    mu_assert("psnr_y after vmaf_sycl_upload_plane() is the CPU's on every 4K frame", same);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_default_model_refused);
    mu_run_test(test_host_picture_features_refused);
    mu_run_test(test_luma_model_runs_and_matches_cpu);
    mu_run_test(test_admitted_twins_run_and_match_cpu);
    mu_run_test(test_upload_plane_orders_compute);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
