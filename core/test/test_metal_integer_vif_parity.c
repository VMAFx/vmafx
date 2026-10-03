/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * vif CPU vs. Metal: every output has the CPU's bits, and the frame sizes the
 * CPU's filters cannot cover go to the CPU (ADR-1435 for the HIP twin;
 * ADR-1381 and ADR-1324 for the minimum frame size).
 *
 * The fixed-point VIF statistic is integer arithmetic up to its last step:
 * int64 accumulators per scale, which integer_vif.c turns into two floats and
 * a single-precision ratio. The Metal twin accumulates the same integers, so
 * it must return the CPU's values; this test asserts equality, not a
 * tolerance, on every output of every case: the four scale scores and, with
 * `debug=true`, the frame ratio and the sums it is formed from. The cases
 * follow test_hip_vif_parity.c (default, debug, 10 and 12 bits,
 * vif_enhn_gain_limit, vif_skip_scale0) and test_hip_vif_min_dim.c.
 *
 * State row measured: T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29. Every scale
 * of integer VIF reflects its filter taps once, which stays inside the plane
 * only while floor(dim / 2^s) exceeds the tap half-width: 16 pixels for the
 * {17, 9, 5, 3} filters. integer_vif_metal declares no 16-pixel minimum (its
 * init() refuses only below 8). Three cases measure it:
 *
 *  - test_vif_metal_declares_min_dim: the twin must carry the ADR-1324
 *    context check, with the CPU `vif` as fallback, and the check must accept
 *    16 pixels and route 15 and below to the CPU.
 *  - test_vif_metal_direct_init_rejects_below_min: a direct request below 16
 *    pixels (no model, so no fallback) fails with -EINVAL from init().
 *  - test_vif_metal_model_boundary: under model dispatch every vif scale of a
 *    frame below 16 pixels equals the CPU `vif`, and from 16x16 up, where the
 *    twin runs, it is equal too. The model is the built-in "vmaf_v0.6.1",
 *    which needs no file, so the tester's working directory does not matter;
 *    only its four VIF features are kept.
 *
 * The first two cases need the twin to refuse what the CPU accepts, so they
 * mean something only on a device (metal_twin_device_only()); the third
 * compares the model path with the CPU extractor and runs in the self-test
 * too.
 *
 * Skip behaviour: without a Metal device every case reports the skip and the
 * run exits 77. The registration case needs no device.
 */

#include "metal_twin.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "feature/feature_extractor.h"
#include "libvmaf/feature.h"
#include "libvmaf/model.h"
#include "libvmaf/picture.h"
#include "model.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define MAX_KEYS 15u
/* Frames per run: the second one shows that a frame starts from cleared
 * accumulators. */
#define NUM_FRAMES 2u
#define NUM_SCALES 4u

static const char *const TWIN_NAME = METAL_TWIN("integer_vif_metal", "vif");

/* One comparison: a bit depth, at most one option, and the keys to compare. */
typedef struct VifCase {
    const char *name;
    unsigned bpc;
    const char *option; /* NULL, or an option name */
    const char *value;  /* the option's value */
    size_t n_keys;
    const char *keys[MAX_KEYS];
} VifCase;

/* The four scores under their registered names, and under the names an
 * option set derives from the aliases ("integer_vif_scale0" + suffix). */
#define SCALE_KEYS                                                                                 \
    "VMAF_integer_feature_vif_scale0_score", "VMAF_integer_feature_vif_scale1_score",              \
        "VMAF_integer_feature_vif_scale2_score", "VMAF_integer_feature_vif_scale3_score"

#define OPTION_SCALE_KEYS(suffix)                                                                  \
    "integer_vif_scale0" suffix, "integer_vif_scale1" suffix, "integer_vif_scale2" suffix,         \
        "integer_vif_scale3" suffix

#define DEBUG_KEYS                                                                                 \
    SCALE_KEYS, "integer_vif", "integer_vif_num", "integer_vif_den", "integer_vif_num_scale0",     \
        "integer_vif_den_scale0", "integer_vif_num_scale1", "integer_vif_den_scale1",              \
        "integer_vif_num_scale2", "integer_vif_den_scale2", "integer_vif_num_scale3",              \
        "integer_vif_den_scale3"

static void put_sample(VmafPicture *pic, unsigned plane, unsigned row, unsigned col, unsigned value)
{
    const unsigned peak = (1u << pic->bpc) - 1u;
    uint8_t *line = (uint8_t *)pic->data[plane] + ((size_t)row * (size_t)pic->stride[plane]);
    if (pic->bpc <= 8u) {
        line[col] = (uint8_t)(value & peak);
    } else {
        ((uint16_t *)line)[col] = (uint16_t)(value & peak);
    }
}

/* Luma of the fixture: a wrapping ramp with texture and a frame-dependent
 * phase for the reference; the distorted frame adds a periodic error. The
 * left third is flat, so the statistic takes its low-variance branch there
 * and its logarithm branch, over a wide range of variances, in the rest. */
static unsigned luma(unsigned row, unsigned col, unsigned frame, bool distorted, unsigned gain)
{
    if (col < FIXTURE_W / 3u) {
        return 96u * gain;
    }
    unsigned value = ((row + col + (frame * 7u) + (((row * 5u) ^ (col * 3u)) % 23u)) * gain) +
                     ((row * col) % gain);
    if (distorted) {
        value += (((row * 2u) + col + (frame * 3u)) % 13u) * gain;
    }
    return value;
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
            put_sample(pic, 0u, row, col, luma(row, col, frame, distorted, gain));
        }
    }
    for (unsigned plane = 1; plane < 3; plane++) {
        for (unsigned row = 0; row < pic->h[plane]; row++) {
            for (unsigned col = 0; col < pic->w[plane]; col++) {
                put_sample(pic, plane, row, col, 128u * gain);
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

/* A context with the CPU `vif` extractor, or with the twin on `state`,
 * carrying the case's option. */
static int vif_context(VmafContext **vmaf, void *state, const VifCase *c)
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
        err = vmaf_use_feature(*vmaf, state ? TWIN_NAME : "vif", opts);
    }
    return err;
}

/* NUM_FRAMES frames through one extractor, and every key of the case of every
 * frame read into `out` (frame-major). Returns the first error. */
static int vif_scores(void *state, const VifCase *c, double *out)
{
    VmafContext *vmaf = NULL;
    int err = vif_context(&vmaf, state, c);
    for (unsigned frame = 0; frame < NUM_FRAMES && !err; frame++) {
        err = feed_frame(vmaf, c->bpc, frame);
    }
    if (!err) {
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    }
    for (size_t i = 0; i < c->n_keys * NUM_FRAMES && !err; i++) {
        const char *key = c->keys[i % c->n_keys];
        err = vmaf_feature_score_at_index(vmaf, key, &out[i], (unsigned)(i / c->n_keys));
        if (err) {
            (void)fprintf(stderr, "\n%s: no score for %s (%s)\n", c->name, key,
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
static unsigned exact_mismatches(const VifCase *c)
{
    double cpu[MAX_KEYS * NUM_FRAMES] = {0.0};
    double gpu[MAX_KEYS * NUM_FRAMES] = {0.0};
    void *state = metal_device();
    if (!state) {
        return 0u;
    }
    const int gpu_err = vif_scores(state, c, gpu);
    (void)metal_twin_close(state);
    const int cpu_err = gpu_err ? 0 : vif_scores(NULL, c, cpu);
    if (gpu_err || cpu_err) {
        (void)fprintf(stderr, "\n%s: run failed (%s %d, cpu %d)\n", c->name, METAL_TWIN_BACKEND,
                      gpu_err, cpu_err);
        return UINT32_MAX;
    }
    unsigned mismatches = 0u;
    for (size_t i = 0; i < c->n_keys * NUM_FRAMES; i++) {
        if (isfinite(cpu[i]) && cpu[i] == gpu[i]) {
            continue;
        }
        mismatches++;
        (void)fprintf(stderr, "\n%s %ux%u frame %u %s: cpu=%.17g metal=%.17g delta=%.3e\n", c->name,
                      FIXTURE_W, FIXTURE_H, (unsigned)(i / c->n_keys), c->keys[i % c->n_keys],
                      cpu[i], gpu[i], fabs(cpu[i] - gpu[i]));
    }
    return mismatches;
}

static char *test_vif_metal_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(TWIN_NAME);
    mu_assert("integer_vif_metal extractor must be registered", fex != NULL);
    mu_assert("integer_vif_metal name matches", !strcmp(fex->name, TWIN_NAME));
    return NULL;
}

static char *test_vif_default_exact(void)
{
    static const VifCase c = {.name = "default", .bpc = 8u, .n_keys = 4u, .keys = {SCALE_KEYS}};
    mu_assert("integer_vif_metal is not bit-identical to the CPU vif extractor",
              exact_mismatches(&c) == 0u);
    return NULL;
}

/* debug=true publishes the frame ratio and the sums it is formed from: the
 * accumulators themselves, rounded to float as vif_store_residuals() does. */
static char *test_vif_debug_exact(void)
{
    static const VifCase c = {.name = "debug",
                              .bpc = 8u,
                              .option = "debug",
                              .value = "true",
                              .n_keys = 15u,
                              .keys = {DEBUG_KEYS}};
    mu_assert("integer_vif_metal debug outputs are not the CPU's bit for bit",
              exact_mismatches(&c) == 0u);
    return NULL;
}

/* 10 and 12 bits go through the 16-bit kernels at scale 0, with the rounding
 * shifts of that bit depth. */
static char *test_vif_10bit_exact(void)
{
    static const VifCase c = {.name = "10-bit",
                              .bpc = 10u,
                              .option = "debug",
                              .value = "true",
                              .n_keys = 15u,
                              .keys = {DEBUG_KEYS}};
    mu_assert("integer_vif_metal is not bit-identical to the CPU at 10 bits",
              exact_mismatches(&c) == 0u);
    return NULL;
}

static char *test_vif_12bit_exact(void)
{
    static const VifCase c = {.name = "12-bit", .bpc = 12u, .n_keys = 4u, .keys = {SCALE_KEYS}};
    mu_assert("integer_vif_metal is not bit-identical to the CPU at 12 bits",
              exact_mismatches(&c) == 0u);
    return NULL;
}

/* vif_enhn_gain_limit=1.0 caps the gain at every pixel whose distorted
 * variance exceeds the reference's. */
static char *test_vif_gain_limit_exact(void)
{
    static const VifCase c = {.name = "vif_enhn_gain_limit=1.0",
                              .bpc = 8u,
                              .option = "vif_enhn_gain_limit",
                              .value = "1.0",
                              .n_keys = 4u,
                              .keys = {OPTION_SCALE_KEYS("_egl_1")}};
    mu_assert("integer_vif_metal is not bit-identical to the CPU with vif_enhn_gain_limit=1.0",
              exact_mismatches(&c) == 0u);
    return NULL;
}

/* vif_skip_scale0 publishes 0.0 for scale 0 and the CPU's values for the
 * other three. */
static char *test_vif_skip_scale0_exact(void)
{
    static const VifCase c = {.name = "vif_skip_scale0",
                              .bpc = 8u,
                              .option = "vif_skip_scale0",
                              .value = "true",
                              .n_keys = 4u,
                              .keys = {OPTION_SCALE_KEYS("_ssclz")}};
    mu_assert("integer_vif_metal is not bit-identical to the CPU with vif_skip_scale0",
              exact_mismatches(&c) == 0u);
    return NULL;
}

/* floor(dim / 2^s) >= half_width + 1 for the scale filters {17, 9, 5, 3} and
 * the decimation filters {9, 5, 3} of scales 0-2. */
static unsigned expected_min_dim(void)
{
    static const unsigned widths[NUM_SCALES] = {17u, 9u, 5u, 3u};
    static const unsigned rd_widths[NUM_SCALES] = {9u, 5u, 3u, 0u};
    unsigned min_dim = 1u;
    for (unsigned s = 0; s < NUM_SCALES; s++) {
        const unsigned need = (widths[s] / 2u + 1u) << s;
        const unsigned rd_need = rd_widths[s] ? (rd_widths[s] / 2u + 1u) << s : 1u;
        min_dim = need > min_dim ? need : min_dim;
        min_dim = rd_need > min_dim ? rd_need : min_dim;
    }
    return min_dim;
}

typedef struct Geometry {
    unsigned w;
    unsigned h;
} Geometry;

/* The ADR-1324 declaration itself: a context check and a CPU fallback. */
static char *check_fallback_declared(const VmafFeatureExtractor *fex)
{
    mu_assert("integer_vif_metal must declare an ADR-1324 context check",
              fex->context_check != NULL);
    mu_assert("integer_vif_metal must fall back to the CPU `vif`",
              fex->context_fallback_name && !strcmp(fex->context_fallback_name, "vif"));
    VmafFeatureExtractor *cpu = vmaf_get_feature_extractor_by_name(fex->context_fallback_name);
    const unsigned device_flags = VMAF_FEATURE_EXTRACTOR_CUDA | VMAF_FEATURE_EXTRACTOR_SYCL |
                                  VMAF_FEATURE_EXTRACTOR_HIP | VMAF_FEATURE_EXTRACTOR_METAL;
    mu_assert("the fallback must be a CPU extractor",
              cpu != NULL && (cpu->flags & device_flags) == 0);
    return NULL;
}

/* The twin has to refuse what the CPU accepts: device only. */
static char *test_vif_metal_declares_min_dim(void)
{
    if (metal_twin_device_only() || !metal_twin_have_device()) {
        return NULL;
    }
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(TWIN_NAME);
    mu_assert("integer_vif_metal is not registered", fex != NULL);
    mu_assert_msg(check_fallback_declared(fex));

    const unsigned min_dim = expected_min_dim();
    mu_assert("integer VIF filter footprint is not 16 pixels", min_dim == 16u);
    const Geometry accepted[] = {{min_dim, min_dim}, {min_dim, 4096u}, {1920u, 1080u}};
    const Geometry rejected[] = {{min_dim - 1u, min_dim},
                                 {min_dim, min_dim - 1u},
                                 {min_dim - 1u, min_dim - 1u},
                                 {8u, 8u},
                                 {3u, 3u},
                                 {4096u, 9u}};
    for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i++) {
        mu_assert("integer_vif_metal must accept frames at or above the minimum",
                  fex->context_check(fex, VMAF_PIX_FMT_YUV420P, 8u, accepted[i].w, accepted[i].h) ==
                      0);
    }
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        mu_assert("integer_vif_metal must route frames below the minimum to the CPU",
                  fex->context_check(fex, VMAF_PIX_FMT_YUV420P, 8u, rejected[i].w, rejected[i].h) ==
                      -ENOTSUP);
    }
    return NULL;
}

/* Deterministic noise for the boundary frames. */
static uint8_t noise_sample(unsigned row, unsigned col, unsigned salt)
{
    uint32_t x = ((uint32_t)row << 16) ^ (uint32_t)col ^ (salt * 0x9E3779B9u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return (uint8_t)(x >> 24);
}

static int fill_noise(VmafPicture *pic, Geometry g, unsigned salt)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8u, g.w, g.h);
    if (err) {
        return err;
    }
    for (unsigned p = 0; p < 3u; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                plane[(row * pic->stride[p]) + col] = noise_sample(row, col, salt + (p << 8));
            }
        }
    }
    return 0;
}

/* The status of the first frame of w x h noise sent to the twin by name, with
 * a real Metal state: a direct request has no fallback, so init() answers. */
static int direct_request_status(void *state, Geometry g, bool *setup_failed)
{
    VmafContext *vmaf = NULL;
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafPicture ref;
    VmafPicture dist;
    int err = vmaf_init(&vmaf, cfg);
    if (!err) {
        err = metal_twin_import(vmaf, state);
    }
    if (!err) {
        err = vmaf_use_feature(vmaf, TWIN_NAME, NULL);
    }
    if (!err) {
        err = fill_noise(&ref, g, 1u);
    }
    if (!err) {
        err = fill_noise(&dist, g, 101u);
        if (err) {
            (void)vmaf_picture_unref(&ref);
        }
    }
    *setup_failed = err != 0;
    if (!err) {
        /* Only this call runs init(): its status is the twin's answer. */
        err = vmaf_read_pictures(vmaf, &ref, &dist, 0u);
    }
    if (vmaf) {
        (void)vmaf_close(vmaf);
    }
    return err;
}

static char *test_vif_metal_direct_init_rejects_below_min(void)
{
    if (metal_twin_device_only()) {
        return NULL;
    }
    void *state = metal_device();
    if (!state) {
        return NULL;
    }
    const Geometry rejected[] = {{15u, 15u}, {15u, 64u}, {64u, 15u}, {8u, 8u}};
    int rc = -EINVAL;
    size_t bad = 0u;
    bool setup_failed = false;
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]) && rc == -EINVAL; i++) {
        rc = direct_request_status(state, rejected[i], &setup_failed);
        bad = i;
        if (setup_failed) {
            break;
        }
    }
    (void)metal_twin_close(state);
    mu_assert("direct request: context setup failed (not init's refusal)", !setup_failed);
    if (rc != -EINVAL) {
        (void)fprintf(stderr, "\n%ux%u: direct request returned %d\n", rejected[bad].w,
                      rejected[bad].h, rc);
    }
    mu_assert("direct integer_vif_metal must reject frames below the minimum with -EINVAL",
              rc == -EINVAL);
    return NULL;
}

/* Feed NUM_FRAMES noise pairs, flush, read every scale of every frame. */
static char *score_noise_frames(VmafContext *vmaf, Geometry g, double out[NUM_FRAMES][NUM_SCALES])
{
    static const char *const keys[NUM_SCALES] = {SCALE_KEYS};
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        mu_assert("reference picture allocation failed", !fill_noise(&ref, g, 1u + i));
        if (fill_noise(&dist, g, 101u + i)) {
            (void)vmaf_picture_unref(&ref);
            return "distorted picture allocation failed";
        }
        mu_assert("vmaf_read_pictures failed", !vmaf_read_pictures(vmaf, &ref, &dist, i));
    }
    mu_assert("vmaf_read_pictures(EOS) failed", !vmaf_read_pictures(vmaf, NULL, NULL, 0));
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        for (unsigned k = 0; k < NUM_SCALES; k++) {
            mu_assert("VIF score missing",
                      !vmaf_feature_score_at_index(vmaf, keys[k], &out[i][k], i));
        }
    }
    return NULL;
}

static char *noise_cpu(Geometry g, double out[NUM_FRAMES][NUM_SCALES])
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("CPU: vmaf_init failed", !vmaf_init(&vmaf, cfg));
    char *msg = NULL;
    if (vmaf_use_feature(vmaf, "vif", NULL)) {
        msg = "CPU: vmaf_use_feature(vif) failed";
    } else {
        msg = score_noise_frames(vmaf, g, out);
    }
    (void)vmaf_close(vmaf);
    return msg;
}

/* Keep only the model's VIF features, in front, and return how many there
 * were so the caller can restore n_features before vmaf_model_destroy(). */
static unsigned keep_vif_features(VmafModel *model)
{
    const unsigned total = model->n_features;
    unsigned kept = 0;
    for (unsigned i = 0; i < total; i++) {
        if (strstr(model->feature[i].name, "vif")) {
            const VmafModelFeature tmp = model->feature[kept];
            model->feature[kept] = model->feature[i];
            model->feature[i] = tmp;
            kept++;
        }
    }
    model->n_features = kept;
    return total;
}

/* The model path: the one that may replace the twin with the CPU `vif`. */
static char *noise_model(void *state, Geometry g, double out[NUM_FRAMES][NUM_SCALES])
{
    VmafModel *model = NULL;
    VmafModelConfig model_cfg = {0};
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    char *msg = NULL;
    unsigned total_features = 0;
    if (vmaf_model_load(&model, &model_cfg, "vmaf_v0.6.1")) {
        msg = "Metal: loading vmaf_v0.6.1 failed";
    } else {
        total_features = keep_vif_features(model);
        if (model->n_features != NUM_SCALES) {
            msg = "Metal: vmaf_v0.6.1 no longer carries four VIF features";
        } else if (vmaf_init(&vmaf, cfg) || metal_twin_import(vmaf, state)) {
            msg = "Metal: context setup failed";
        } else if (vmaf_use_features_from_model(vmaf, model)) {
            msg = "Metal: vmaf_use_features_from_model failed";
        } else {
            msg = score_noise_frames(vmaf, g, out);
        }
    }
    if (vmaf) {
        (void)vmaf_close(vmaf);
    }
    if (model) {
        model->n_features = total_features ? total_features : model->n_features;
        vmaf_model_destroy(model);
    }
    return msg;
}

/* Every scale of every frame equals the CPU's: below the minimum because the
 * CPU extractor runs there, from it up because the twin returns its bits. */
static char *check_boundary_case(void *state, Geometry g)
{
    double cpu[NUM_FRAMES][NUM_SCALES];
    double gpu[NUM_FRAMES][NUM_SCALES];
    char *msg = noise_cpu(g, cpu);
    if (msg) {
        return msg;
    }
    msg = noise_model(state, g, gpu);
    if (msg) {
        return msg;
    }
    const bool on_cpu = g.w < expected_min_dim() || g.h < expected_min_dim();
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        for (unsigned k = 0; k < NUM_SCALES; k++) {
            if (cpu[i][k] == gpu[i][k]) {
                continue;
            }
            (void)fprintf(stderr, "\n  %ux%u frame %u scale %u: cpu=%.17g model=%.17g\n", g.w, g.h,
                          i, k, cpu[i][k], gpu[i][k]);
            return on_cpu ? "below the minimum the model path must run the CPU `vif`" :
                            "above the minimum integer_vif_metal must match the CPU `vif`";
        }
    }
    return NULL;
}

static char *test_vif_metal_model_boundary(void)
{
    void *state = metal_device();
    if (!state) {
        return NULL;
    }
    const unsigned m = expected_min_dim();
    /* 853x480 has odd widths below scale 0 (853 -> 426 -> 213). */
    const Geometry cases[] = {{m, m},   {m - 1u, m - 1u}, {m - 1u, 64u}, {64u, m - 1u},
                              {8u, 8u}, {m + 1u, m + 1u}, {96u, 64u},    {853u, 480u}};
    char *msg = NULL;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]) && !msg; i++) {
        msg = check_boundary_case(state, cases[i]);
    }
    (void)metal_twin_close(state);
    return msg;
}

static void run_exact_cases(void)
{
    metal_run_case(test_vif_default_exact);
    metal_run_case(test_vif_debug_exact);
    metal_run_case(test_vif_10bit_exact);
    metal_run_case(test_vif_12bit_exact);
    metal_run_case(test_vif_gain_limit_exact);
    metal_run_case(test_vif_skip_scale0_exact);
}

static void run_min_dim_cases(void)
{
    metal_run_case(test_vif_metal_declares_min_dim);
    metal_run_case(test_vif_metal_direct_init_rejects_below_min);
    metal_run_case(test_vif_metal_model_boundary);
}

char *run_tests(void)
{
    metal_run_case(test_vif_metal_registered);
    run_exact_cases();
    run_min_dim_cases();
    return metal_first_failure;
}

/* NOLINTEND(modernize-use-nullptr) */
