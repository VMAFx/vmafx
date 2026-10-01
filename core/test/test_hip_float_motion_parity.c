/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-0945 round-3 — float_motion CPU vs. HIP parity test.
 *
 * The float-pipeline motion feature is computed by float_motion.c (CPU)
 * and by float_motion_hip.c + float_motion/motion_score.hip (HIP).
 * Round-2 (PR #372) covered the integer motion v1 (motion_hip); this
 * test closes the float twin.  Motion requires two frames (the score
 * at index 0 is forced to zero by spec — the t-1 reference doesn't
 * exist for the first frame), so we assert against index 1.
 *
 * Since ADR-1409 the twin adds its SAD in the CPU's order, and
 * test_float_motion_matches_cpu_bit_for_bit holds motion, motion2 and motion3
 * of every frame to the CPU's value bit for bit, with the default options and
 * with motion_add_scale1 + motion_add_uv. The tolerance test stays as the
 * coarse gate.
 *
 * Skip behaviour: if vmaf_hip_state_init() fails (no HIP runtime / no
 * device) OR the HIP path returns -ENOSYS (scaffold posture under
 * enable_hipcc=false) the test emits a skip-tag and passes.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "hip_parity_skip.h"

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "feature/feature_name.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"
#include "picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): this is a
 * C23 translation unit, but the required MSVC C lane does not provide the C
 * nullptr spelling clang-tidy proposes. Keep the portable C API form under
 * ADR-1138. */

#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#define FIXTURE_BPC 8u
#define PARITY_TOL 1e-4

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + salt * 23u) & 0xFFu);
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

static int feed_two_frames(VmafContext *vmaf)
{
    for (unsigned i = 0; i < 2; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_pic(&ref, i);
        if (err)
            return err;
        err = fill_pic(&dist, i + 1u);
        if (err) {
            vmaf_picture_unref(&ref);
            return err;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err)
            return err;
    }
    return 0;
}

static char *run_cpu_float_motion(double *score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "float_motion", NULL);
    mu_assert("CPU: vmaf_use_feature(float_motion) failed", !err);
    err = feed_two_frames(vmaf);
    mu_assert("CPU: feed_two_frames failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "VMAF_feature_motion_score", score, 1u);
    mu_assert("CPU: VMAF_feature_motion_score missing", !err);
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static int collect_hip_float_motion(VmafContext *vmaf, double *score, const char **skip_where)
{
    int err = feed_two_frames(vmaf);
    if (err != 0) {
        *skip_where = " on feed";
        return err;
    }

    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (err != 0) {
        *skip_where = " on EOS";
        return err;
    }

    *skip_where = " on score";
    return vmaf_feature_score_at_index(vmaf, "VMAF_feature_motion_score", score, 1u);
}

static char *run_hip_float_motion(double *score, int *skipped)
{
    *score = NAN;
    *skipped = 0;
    VmafHipState *hip_state = NULL;
    VmafHipConfiguration hip_cfg = {.device_index = -1};
    int err = vmaf_hip_state_init(&hip_state, hip_cfg);
    if (err != 0 || hip_state == NULL) {
        (void)fprintf(stderr, "[skip: no HIP device] ");
        *skipped = 1;
        return NULL;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("HIP: vmaf_init failed", !err);
    err = vmaf_hip_import_state(vmaf, hip_state);
    mu_assert("HIP: vmaf_hip_import_state failed", !err);
    err = vmaf_use_feature(vmaf, "float_motion_hip", NULL);
    if (err == -ENOSYS)
        return hip_parity_skip(vmaf, &hip_state, skipped, "");
    mu_assert("HIP: vmaf_use_feature(float_motion_hip) failed", !err);

    const char *skip_where = "";
    err = collect_hip_float_motion(vmaf, score, &skip_where);
    if (err == -ENOSYS)
        return hip_parity_skip(vmaf, &hip_state, skipped, skip_where);
    mu_assert("HIP: float-motion collection failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("HIP: vmaf_close failed", !err);
    vmaf_hip_state_free(&hip_state);
    return NULL;
}

static char *test_float_motion_hip_registered(void)
{
    const VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_motion_hip");
    mu_assert("float_motion_hip extractor must be registered", fex != NULL);
    mu_assert("float_motion_hip name matches", !strcmp(fex->name, "float_motion_hip"));
    return NULL;
}

static int hip_device_available(void)
{
    VmafHipState *state = NULL;
    const VmafHipConfiguration cfg = {.device_index = -1};
    const int err = vmaf_hip_state_init(&state, cfg);
    const int available = err == 0 && state != NULL;
    vmaf_hip_state_free(&state);
    return available;
}

static int create_float_motion_hip_context(VmafFeatureExtractorContext **ctx,
                                           VmafFeatureExtractor **registered_fex, int force_zero)
{
    *registered_fex = vmaf_get_feature_extractor_by_name("float_motion_hip");
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
    int err = create_float_motion_hip_context(&ctx, &registered_fex, 1);
    if (err == 0) {
        *kept_close = ctx->fex->close != NULL;
        if (!*kept_close)
            ctx->fex->close = registered_fex->close;
    }
    return cleanup_float_motion_context(ctx, err);
}

static char *test_float_motion_hip_force_zero_keeps_close(void)
{
    if (!hip_device_available()) {
        (void)fprintf(stderr, "[skip: no HIP device] ");
        return NULL;
    }

    int kept_close = 0;
    const int err = probe_force_zero_close(&kept_close);
    mu_assert("HIP force-zero context lifecycle failed", err == 0);
    mu_assert("HIP force-zero init must retain a dictionary-owning close callback", kept_close);
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
    /* The CPU float_motion emits the debug motion score and the flushed tail
     * motion2 both through motion_clip(), which applies motion_fps_weight
     * (1.5 here) once; the HIP twin does the same since ADR-1382, so the tail
     * equals the last frame's debug score, not 1.5 times it. */
    result->expected_tail = current_motion;

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
    int err = create_float_motion_hip_context(&ctx, &registered_fex, 0);
    VmafFeatureCollector *fc = NULL;
    VmafPicture pics[2] = {0};
    if (err == 0)
        err = vmaf_feature_collector_init(&fc);
    if (err == 0)
        err = fill_pic(&pics[0], 0u);
    if (err == 0)
        err = fill_pic(&pics[1], 1u);
    if (err == 0)
        err = feed_flush_probe(ctx, fc, pics, result);
    return cleanup_flush_probe(ctx, fc, pics, err);
}

static char *test_float_motion_hip_flush_is_idempotent(void)
{
    if (!hip_device_available()) {
        (void)fprintf(stderr, "[skip: no HIP device] ");
        return NULL;
    }

    FlushProbeResult result = {
        .expected_tail = NAN,
        .first_tail = NAN,
        .retained_tail = NAN,
    };
    const int err = probe_flush_idempotency(&result);
    if (result.skipped) {
        (void)fprintf(stderr, "[skip: HIP scaffold ENOSYS on direct submit] ");
        return NULL;
    }
    mu_assert("HIP float-motion context lifecycle failed", err == 0);
    mu_assert("HIP float-motion first tail flush must finish", result.first_flush == 1);
    mu_assert("HIP float-motion tail must equal weighted final motion",
              result.first_tail == result.expected_tail);
    mu_assert("HIP float-motion repeated tail flush must be idempotent", result.second_flush == 1);
    mu_assert("HIP float-motion repeated flush must retain the tail value",
              result.retained_tail == result.expected_tail);
    return NULL;
}

static char *test_float_motion_cpu_hip_parity(void)
{
    double cpu = 0.0;
    double gpu = NAN;
    int skipped = 0;

    char *msg = run_cpu_float_motion(&cpu);
    if (msg)
        return msg;
    msg = run_hip_float_motion(&gpu, &skipped);
    if (msg)
        return msg;
    if (skipped || isnan(gpu))
        return NULL;
    double delta = fabs(cpu - gpu);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\nfloat_motion parity FAIL: cpu=%.8f hip=%.8f delta=%.2e tol=%.2e\n",
                      cpu, gpu, delta, PARITY_TOL);
    }
    mu_assert("float_motion CPU vs. HIP delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* ADR-1409 — float_motion_hip returns the CPU extractor's bits.          */
/*                                                                     */
/* float_motion.c adds the absolute differences of a row into one fp32  */
/* accumulator, the rows into another, and divides in fp32. The twin    */
/* used to add 16x16 blocks on the device and the blocks in double on   */
/* the host, and was 3e-6 (576x324) to 1.4e-4 (1080p checkerboards)     */
/* from the CPU on a gfx1036. It now stores every absolute difference   */
/* and adds each row in the CPU's order (float_motion/                  */
/* float_motion_rows.h), also for the half-size term of                 */
/* motion_add_scale1 and for the chroma planes of motion_add_uv.        */
/* ------------------------------------------------------------------ */
#define FM_EXACT_FRAMES 4u
#define FM_EXACT_KEYS 3u

typedef struct FmExactScores {
    double v[FM_EXACT_FRAMES][FM_EXACT_KEYS];
} FmExactScores;

static const char *const fm_exact_keys[FM_EXACT_KEYS] = {
    "VMAF_feature_motion_score",
    "VMAF_feature_motion2_score",
    "VMAF_feature_motion3_score",
};

/* Texture and noise that move from frame to frame on all three planes, so the
 * absolute differences have full mantissas and the running sums round. */
static int fm_exact_fill(VmafPicture *pic, unsigned frame)
{
    const int err =
        vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint32_t state = 0x9E3779B9u ^ (frame * 2654435761u);
    for (unsigned p = 0; p < 3u; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                state = (state * 1664525u) + 1013904223u;
                const unsigned value =
                    (((col * 5u) + (row * 3u) + (frame * 17u)) & 127u) + 48u + ((state >> 8) & 63u);
                plane[(row * pic->stride[p]) + col] = (uint8_t)value;
            }
        }
    }
    return 0;
}

/* An initialised context of extractor `name`, with motion_add_scale1 and
 * motion_add_uv when `options` is set. */
static int fm_exact_context(VmafFeatureExtractorContext **ctx, const char *name, bool options)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(name);
    if (fex == NULL)
        return -ENOENT;
    VmafDictionary *opts = NULL;
    int err = 0;
    if (options)
        err = vmaf_dictionary_set(&opts, "motion_add_scale1", "true", 0);
    if (!err && options)
        err = vmaf_dictionary_set(&opts, "motion_add_uv", "true", 0);
    if (!err)
        err = vmaf_feature_extractor_context_create(ctx, fex, opts);
    if (err) {
        (void)vmaf_dictionary_free(&opts);
        return err;
    }
    return vmaf_feature_extractor_context_init(*ctx, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W,
                                               FIXTURE_H);
}

/* One frame through the extractor: extract() on the CPU, submit() and
 * collect() on the twin. */
static int fm_exact_frame(VmafFeatureExtractorContext *ctx, VmafFeatureCollector *fc,
                          unsigned frame)
{
    VmafPicture pic;
    int err = fm_exact_fill(&pic, frame);
    if (err)
        return err;
    if (ctx->fex->extract != NULL) {
        err = vmaf_feature_extractor_context_extract(ctx, &pic, NULL, &pic, NULL, frame, fc);
    } else {
        err = submit_and_collect(ctx, fc, &pic, frame);
    }
    return merge_cleanup_error(err, vmaf_picture_unref(&pic));
}

static int fm_exact_collect(const VmafFeatureExtractorContext *ctx, VmafFeatureCollector *fc,
                            FmExactScores *out)
{
    for (unsigned i = 0; i < FM_EXACT_FRAMES; i++) {
        for (unsigned k = 0; k < FM_EXACT_KEYS; k++) {
            const int err = get_option_resolved_score(ctx, fc, fm_exact_keys[k], i, &out->v[i][k]);
            if (err)
                return err;
        }
    }
    return 0;
}

/* Every frame's motion, motion2 and motion3 from extractor `name`. */
static int fm_exact_scores(const char *name, bool options, FmExactScores *out)
{
    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    int err = fm_exact_context(&ctx, name, options);
    if (!err)
        err = vmaf_feature_collector_init(&fc);
    for (unsigned i = 0; i < FM_EXACT_FRAMES && !err; i++)
        err = fm_exact_frame(ctx, fc, i);
    if (!err) {
        const int flushed = vmaf_feature_extractor_context_flush(ctx, fc);
        err = (flushed < 0) ? flushed : 0;
    }
    if (!err)
        err = fm_exact_collect(ctx, fc, out);
    err = cleanup_float_motion_context(ctx, err);
    if (fc != NULL)
        vmaf_feature_collector_destroy(fc);
    vmaf_picture_pool_flush();
    return err;
}

static uint64_t fm_exact_bits(double v)
{
    uint64_t bits = 0u;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

/* The scores that are not the CPU's bit for bit, each one reported. */
static unsigned fm_exact_mismatches(const FmExactScores *cpu, const FmExactScores *gpu)
{
    unsigned differing = 0u;
    for (unsigned i = 0; i < FM_EXACT_FRAMES; i++) {
        for (unsigned k = 0; k < FM_EXACT_KEYS; k++) {
            if (fm_exact_bits(cpu->v[i][k]) == fm_exact_bits(gpu->v[i][k]))
                continue;
            differing++;
            (void)fprintf(stderr, "\n%s frame %u: cpu=%.17g hip=%.17g delta=%.3e", fm_exact_keys[k],
                          i, cpu->v[i][k], gpu->v[i][k], fabs(cpu->v[i][k] - gpu->v[i][k]));
        }
    }
    return differing;
}

/* 0 when the twin's scores are the CPU's, the number of differing scores
 * otherwise; `*status` is the first error, -ENOSYS for a scaffold build. */
static unsigned fm_exact_compare(bool options, int *status)
{
    FmExactScores cpu;
    FmExactScores gpu;
    memset(&cpu, 0, sizeof(cpu));
    memset(&gpu, 0, sizeof(gpu));
    *status = fm_exact_scores("float_motion_hip", options, &gpu);
    if (*status == 0)
        *status = fm_exact_scores("float_motion", options, &cpu);
    if (*status != 0)
        return 0u;
    if (!(cpu.v[1][0] > 0.0))
        *status = -ERANGE;
    return fm_exact_mismatches(&cpu, &gpu);
}

static char *test_float_motion_matches_cpu_bit_for_bit(void)
{
    if (!hip_device_available()) {
        (void)fprintf(stderr, "[skip: no HIP device] ");
        return NULL;
    }
    int status = 0;
    const unsigned plain = fm_exact_compare(false, &status);
    if (status == -ENOSYS) {
        (void)fprintf(stderr, "[skip: HIP scaffold ENOSYS] ");
        return NULL;
    }
    mu_assert("float_motion: the CPU or the HIP run failed", status == 0);
    mu_assert("float_motion_hip is not the CPU extractor's value bit for bit", plain == 0u);
    const unsigned with_options = fm_exact_compare(true, &status);
    mu_assert("float_motion with scale1 and uv: the CPU or the HIP run failed", status == 0);
    mu_assert("float_motion_hip with motion_add_scale1 and motion_add_uv is not the CPU's value",
              with_options == 0u);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_motion_hip_registered);
    mu_run_test(test_float_motion_cpu_hip_parity);
    mu_run_test(test_float_motion_matches_cpu_bit_for_bit);
    mu_run_test(test_float_motion_hip_flush_is_idempotent);
    mu_run_test(test_float_motion_hip_force_zero_keeps_close);
    return NULL;
}
/* NOLINTEND(modernize-use-nullptr) */
