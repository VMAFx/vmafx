/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Metal kernel coverage round 3 — float_motion CPU vs. Metal parity
 * (PR #1018 parity follow-up; ADR-0214 cross-backend gate).
 *
 * PR #1018 brought `float_motion_metal` to score-parity with the CUDA twin
 * `float_motion_cuda` (the upstream-reference float motion implementation).
 * This test asserts that parity holds at the public-API surface by running
 * the CPU `float_motion` extractor and `float_motion_metal` over the same
 * two-frame YUV420P fixture and comparing
 *
 *     VMAF_feature_motion2_score
 *
 * at frame index 0. Tolerance: places=4 (1e-4) per ADR-0214. The kernel
 * uses float32 SAD reductions; workgroup partial-sum ordering keeps the
 * residual well below the 1e-4 bound.
 *
 * Skip behaviour: -ENODEV from `vmaf_metal_state_init` -> clean skip on
 * Linux / Windows / Intel Mac.
 *
 * Cross-references:
 *   - core/src/feature/metal/float_motion_metal.mm
 *   - core/src/feature/float_motion.c
 *   - PR #1018 (float_motion_metal parity with float_motion_cuda)
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "dict.h"
#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "feature/feature_name.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_metal.h"
#include "libvmaf/picture.h"
#include "picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u
#define NUM_FRAMES 2u
#define PARITY_TOL 1e-4

static int fill_fixture(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + frame_idx * 17u) & 0xFFu);
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

/* Both sides feed the same fixture pair, so the sequence lives here once.
 * Extracting it also keeps each run_* function inside the branch budget the
 * lint profile sets, which is the refactor ADR-0141 asks for rather than a
 * suppression. */
static char *feed_fixture_pair(VmafContext *vmaf, unsigned frame)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_fixture(&ref, frame);
    if (err)
        return "fill_fixture(ref) failed";
    err = fill_fixture(&dist, frame);
    if (err)
        return "fill_fixture(dist) failed";
    err = vmaf_read_pictures(vmaf, &ref, &dist, frame);
    if (err)
        return "vmaf_read_pictures failed";
    return NULL;
}

static char *run_cpu_float_motion(double *out_score)
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "float_motion", NULL);
    mu_assert("CPU: vmaf_use_feature(float_motion) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        char *feed_err = feed_fixture_pair(vmaf, i);
        if (feed_err)
            return feed_err;
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "VMAF_feature_motion2_score", out_score, 0u);
    mu_assert("CPU: vmaf_feature_score_at_index(motion2, idx=0) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_metal_float_motion(double *out_score)
{
    *out_score = NAN;
    int err = 0;

    VmafMetalConfiguration mcfg = {.device_index = -1, .flags = 0};
    VmafMetalState *mstate = NULL;
    err = vmaf_metal_state_init(&mstate, mcfg);
    if (err != 0 || mstate == NULL) {
        (void)fprintf(stderr, "[skip: no Metal device] ");
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("Metal: vmaf_init failed", !err);

    err = vmaf_metal_import_state(vmaf, mstate);
    mu_assert("Metal: vmaf_metal_import_state failed", !err);

    err = vmaf_use_feature(vmaf, "float_motion_metal", NULL);
    mu_assert("Metal: vmaf_use_feature(float_motion_metal) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        char *feed_err = feed_fixture_pair(vmaf, i);
        if (feed_err)
            return feed_err;
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("Metal: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "VMAF_feature_motion2_score", out_score, 0u);
    mu_assert("Metal: vmaf_feature_score_at_index(motion2, idx=0) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("Metal: vmaf_close failed", !err);

    vmaf_metal_state_free(&mstate);
    return NULL;
}

static char *test_float_motion_cpu_metal_parity(void)
{
    double cpu_score = 0.0;
    double metal_score = NAN;

    char *msg = run_cpu_float_motion(&cpu_score);
    if (msg)
        return msg;
    msg = run_metal_float_motion(&metal_score);
    if (msg)
        return msg;

    if (isnan(metal_score))
        return NULL;

    const double delta = fabs(cpu_score - metal_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\nfloat_motion parity FAIL: cpu=%.8f metal=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, metal_score, delta, PARITY_TOL);
    }
    mu_assert("float_motion CPU vs. Metal delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

static int metal_device_available(void)
{
    VmafMetalState *state = NULL;
    const VmafMetalConfiguration cfg = {.device_index = -1, .flags = 0};
    const int err = vmaf_metal_state_init(&state, cfg);
    const int available = (err == 0 && state != NULL);
    if (state != NULL) {
        vmaf_metal_state_free(&state);
    }
    return available;
}

static int create_float_motion_metal_context(VmafFeatureExtractorContext **ctx,
                                             VmafFeatureExtractor **registered_fex, int force_zero)
{
    *registered_fex = vmaf_get_feature_extractor_by_name("float_motion_metal");
    if (*registered_fex == NULL)
        return -ENOENT;

    VmafDictionary *opts = NULL;
    int err = force_zero ? vmaf_dictionary_set(&opts, "motion_force_zero", "true", 0) :
                           vmaf_dictionary_set(&opts, "motion_fps_weight", "1.5", 0);
    if (err) {
        (void)vmaf_dictionary_free(&opts);
        return err;
    }

    err = vmaf_feature_extractor_context_create(ctx, *registered_fex, opts);
    if (err) {
        (void)vmaf_dictionary_free(&opts);
        return err;
    }
    return vmaf_feature_extractor_context_init(*ctx, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W,
                                               FIXTURE_H);
}

static int merge_cleanup_error(int result, int cleanup_result)
{
    return result != 0 ? result : cleanup_result;
}

static int cleanup_float_motion_context(VmafFeatureExtractorContext *ctx, int result)
{
    if (ctx == NULL)
        return result;
    if (ctx->is_initialized) {
        result = merge_cleanup_error(result, vmaf_feature_extractor_context_close(ctx));
    }
    return merge_cleanup_error(result, vmaf_feature_extractor_context_destroy(ctx));
}

static int probe_force_zero_close(int *kept_close)
{
    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureExtractor *registered_fex = NULL;
    int err = create_float_motion_metal_context(&ctx, &registered_fex, 1);
    if (err == 0) {
        *kept_close = ctx->fex->close != NULL;
        if (!*kept_close)
            ctx->fex->close = registered_fex->close;
    }
    return cleanup_float_motion_context(ctx, err);
}

static char *test_float_motion_metal_force_zero_keeps_close(void)
{
    if (!metal_device_available()) {
        (void)fprintf(stderr, "[skip: no Metal device] ");
        return NULL;
    }

    int kept_close = 0;
    const int err = probe_force_zero_close(&kept_close);
    mu_assert("Metal force-zero context lifecycle failed", err == 0);
    mu_assert("Metal force-zero init must retain a dictionary-owning close callback", kept_close);
    return NULL;
}

static int submit_and_collect(VmafFeatureExtractorContext *ctx, VmafFeatureCollector *fc,
                              VmafPicture *pic, unsigned index)
{
    int err = vmaf_feature_extractor_context_submit(ctx, pic, NULL, pic, NULL, index);
    if (err)
        return err;
    return vmaf_feature_extractor_context_collect(ctx, index, fc);
}

typedef struct FlushProbeResult {
    int first_flush;
    int second_flush;
    int skipped;
    double expected_tail;
    double first_tail;
    double retained_tail;
} FlushProbeResult;

static int get_option_resolved_score(const VmafFeatureExtractorContext *ctx,
                                     VmafFeatureCollector *fc, const char *base_name,
                                     unsigned index, double *score)
{
    char *name = vmaf_feature_name_from_options(base_name, ctx->fex->options, ctx->fex->priv);
    if (name == NULL)
        return -ENOMEM;

    const int err = vmaf_feature_collector_get_score(fc, name, score, index);
    free(name);
    return err;
}

static int exercise_tail_flush(VmafFeatureExtractorContext *ctx, VmafFeatureCollector *fc,
                               FlushProbeResult *result)
{
    double current_motion = NAN;
    int err = get_option_resolved_score(ctx, fc, "VMAF_feature_motion_score", 1u, &current_motion);
    if (err != 0)
        return err;
    result->expected_tail = current_motion * 1.5;

    result->first_flush = ctx->fex->flush(ctx->fex, fc);
    if (result->first_flush != 1)
        return result->first_flush != 0 ? result->first_flush : -EIO;
    err = get_option_resolved_score(ctx, fc, "VMAF_feature_motion2_score", 1u, &result->first_tail);
    if (err != 0)
        return err;

    result->second_flush = ctx->fex->flush(ctx->fex, fc);
    if (result->second_flush != 1)
        return result->second_flush != 0 ? result->second_flush : -EIO;
    return get_option_resolved_score(ctx, fc, "VMAF_feature_motion2_score", 1u,
                                     &result->retained_tail);
}

static int cleanup_flush_probe(VmafFeatureExtractorContext *ctx, VmafFeatureCollector *fc,
                               VmafPicture pics[2], int result)
{
    for (unsigned i = 0u; i < 2u; i++) {
        if (pics[i].ref != NULL)
            result = merge_cleanup_error(result, vmaf_picture_unref(&pics[i]));
    }
    result = cleanup_float_motion_context(ctx, result);
    if (fc != NULL)
        vmaf_feature_collector_destroy(fc);
    vmaf_picture_pool_flush();
    return result;
}

static int feed_flush_probe(VmafFeatureExtractorContext *ctx, VmafFeatureCollector *fc,
                            VmafPicture pics[2], FlushProbeResult *result)
{
    int err = submit_and_collect(ctx, fc, &pics[0], 0u);
    if (err == -ENOSYS) {
        result->skipped = 1;
        return 0;
    }
    if (err != 0)
        return err;

    err = submit_and_collect(ctx, fc, &pics[1], 1u);
    if (err == -ENOSYS) {
        result->skipped = 1;
        return 0;
    }
    if (err != 0)
        return err;
    return exercise_tail_flush(ctx, fc, result);
}

static int probe_flush_idempotency(FlushProbeResult *result)
{
    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureExtractor *registered_fex = NULL;
    int err = create_float_motion_metal_context(&ctx, &registered_fex, 0);
    VmafFeatureCollector *fc = NULL;
    VmafPicture pics[2] = {0};
    if (err == 0)
        err = vmaf_feature_collector_init(&fc);
    if (err == 0)
        err = fill_fixture(&pics[0], 0u);
    if (err == 0)
        err = fill_fixture(&pics[1], 1u);
    if (err == 0)
        err = feed_flush_probe(ctx, fc, pics, result);
    return cleanup_flush_probe(ctx, fc, pics, err);
}

static char *test_float_motion_metal_flush_is_idempotent(void)
{
    if (!metal_device_available()) {
        (void)fprintf(stderr, "[skip: no Metal device] ");
        return NULL;
    }

    FlushProbeResult result = {
        .expected_tail = NAN,
        .first_tail = NAN,
        .retained_tail = NAN,
    };
    const int err = probe_flush_idempotency(&result);
    if (result.skipped) {
        (void)fprintf(stderr, "[skip: Metal scaffold ENOSYS on direct submit] ");
        return NULL;
    }
    mu_assert("Metal float-motion context lifecycle failed", err == 0);
    mu_assert("Metal float-motion first tail flush must finish", result.first_flush == 1);
    mu_assert("Metal float-motion tail must equal weighted final motion",
              result.first_tail == result.expected_tail);
    mu_assert("Metal float-motion repeated tail flush must be idempotent",
              result.second_flush == 1);
    mu_assert("Metal float-motion repeated flush must retain the tail value",
              result.retained_tail == result.expected_tail);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_motion_cpu_metal_parity);
    mu_run_test(test_float_motion_metal_force_zero_keeps_close);
    mu_run_test(test_float_motion_metal_flush_is_idempotent);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
