/**
 *  Copyright 2026 luoye-cpu
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * adm CPU vs. Vulkan parity (ADR-3402, the integer path's acceptance gate).
 *
 * The Vulkan twin `adm_vulkan` runs the PoC-validated integer_adm.comp
 * shader — the fp64-free adm_angle_flag_i64() reformulation of the CPU's
 * angle predicate — and ports the CPU's conclude math line for line
 * (conclude_csf_den_host / conclude_cm_host), so this test asserts
 * equality, not a tolerance: every output of every case has the CPU's
 * bits, the per-scale numerators and denominators of `debug=true`
 * included.
 *
 * The shader requires shaderInt64 only, so the twin runs on devices the
 * SYCL float_adm twin cannot serve (Intel iGPUs reporting
 * shaderFloat64 = 0); the PoC validated the same module bit-exact on an
 * RTX 5080, an RTX 4060 Laptop and a Raptor Lake iGPU (issue #3319).
 *
 * Skip behaviour: if vmaf_vulkan_state_init() fails (no Vulkan loader /
 * no device / no shaderInt64) the test emits "[skip: no Vulkan device]"
 * and passes. Besides the synthetic fixtures the gate also runs both twins
 * across the 48 natural-content frames of the shared testdata pairs
 * (576x324 and 640x480), the geometries the SYCL and no-reference oracles
 * already use.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_vulkan.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

/* Fixture geometry — large enough for the 4-scale ADM DWT pyramid. */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif

#define MAX_KEYS 32u

typedef struct Fixture {
    unsigned w;
    unsigned h;
    unsigned bpc;
    bool sparse; /* a flat frame with isolated one-level dots instead of texture */
} Fixture;

/* Deterministic position hash for the sparse fixture. */
static unsigned position_hash(unsigned row, unsigned col, unsigned salt)
{
    unsigned x = row * 73856093u ^ col * 19349663u ^ salt * 83492791u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return x;
}

/* Luma of the textured fixture: a wrapping ramp with texture for the
 * reference; the distorted frame adds a position-dependent error, so every
 * scale has detail to lose and to mask. */
static unsigned textured_luma(unsigned row, unsigned col, bool distorted)
{
    unsigned v = ((row * 3u + col * 2u) & 0xFFu) ^ (((row >> 2) * (col >> 3)) & 0x1Fu);
    if (distorted)
        v += 9u + ((row * 5u + col * 7u) % 11u);
    return v;
}

/* Luma of the sparse fixture: mid grey with one sample in 200 raised by a
 * level, and a distorted frame that raises half of the samples by one more.
 * The reference bands are then almost empty, the denominator accumulators
 * are small integers, and their rounding shows in the score. */
static unsigned sparse_luma(unsigned row, unsigned col, bool distorted)
{
    unsigned v = 128u + (position_hash(row, col, 1u) % 200u == 0u ? 1u : 0u);
    if (distorted)
        v += position_hash(row, col, 2u) & 1u;
    return v;
}

static unsigned fixture_luma(const Fixture *fx, unsigned row, unsigned col, bool distorted)
{
    return fx->sparse ? sparse_luma(row, col, distorted) : textured_luma(row, col, distorted);
}

static void put_sample(VmafPicture *pic, unsigned plane, unsigned row, unsigned col, unsigned v)
{
    const unsigned peak = (1u << pic->bpc) - 1u;
    uint8_t *line = (uint8_t *)pic->data[plane] + (size_t)row * (size_t)pic->stride[plane];
    if (pic->bpc <= 8u) {
        line[col] = (uint8_t)(v > peak ? peak : v);
    } else {
        ((uint16_t *)line)[col] = (uint16_t)(v > peak ? peak : v);
    }
}

static int fill_picture(VmafPicture *pic, const Fixture *fx, bool distorted)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, fx->bpc, fx->w, fx->h);
    if (err)
        return err;
    const unsigned gain = 1u << (fx->bpc - 8u);
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const unsigned v = fixture_luma(fx, row, col, distorted);
            put_sample(pic, 0u, row, col, v * gain);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++)
                put_sample(pic, p, row, col, 128u * gain);
        }
    }
    return 0;
}

static char *feed_one_frame(VmafContext *vmaf, const Fixture *fx)
{
    VmafPicture ref;
    VmafPicture dist;
    mu_assert("fill reference failed", !fill_picture(&ref, fx, false));
    mu_assert("fill distorted failed", !fill_picture(&dist, fx, true));
    mu_assert("vmaf_read_pictures failed", !vmaf_read_pictures(vmaf, &ref, &dist, 0u));
    return NULL;
}

/* One scoring run of the CPU `adm` or the Vulkan `adm_vulkan` extractor. */
typedef struct {
    bool use_vulkan;
    VmafContext *vmaf;
    VmafVulkanState *vk_state;
} AdmRun;

/* Create the context (and the Vulkan state for the Vulkan twin). `*skipped`
 * is set when the Vulkan leg finds no device. */
static char *adm_run_init(AdmRun *run, int *skipped)
{
    if (run->use_vulkan) {
        VmafVulkanConfiguration vk_cfg = {.device_index = -1, .flags = 0};
        if (vmaf_vulkan_state_init(&run->vk_state, vk_cfg) != 0 || run->vk_state == NULL) {
            (void)fprintf(stderr, "[skip: no Vulkan device] ");
            mu_skipped = 1;
            *skipped = 1;
            return NULL;
        }
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    mu_assert("vmaf_init failed", !vmaf_init(&run->vmaf, cfg));
    if (run->use_vulkan) {
        mu_assert("vmaf_vulkan_import_state failed",
                  !vmaf_vulkan_import_state(run->vmaf, run->vk_state));
    }
    return NULL;
}

/* Open the run with `opts` (NULL for defaults). vmaf_use_feature() consumes
 * the dictionary; it is freed here when the run never gets that far. */
static char *adm_run_open(AdmRun *run, VmafFeatureDictionary *opts, int *skipped)
{
    *skipped = 0;
    char *msg = adm_run_init(run, skipped);
    if (msg || *skipped) {
        (void)vmaf_feature_dictionary_free(&opts);
        return msg;
    }
    const char *name = run->use_vulkan ? "adm_vulkan" : "adm";
    const int err = vmaf_use_feature(run->vmaf, name, opts);
    if (err) {
        (void)vmaf_feature_dictionary_free(&opts);
    }
    mu_assert("vmaf_use_feature failed", !err);
    return NULL;
}

/* Feed the fixture frame, flush, and read every key in `keys`. */
static char *adm_run_score(const AdmRun *run, const Fixture *fx, const char *const *keys,
                           size_t count, double *out)
{
    char *msg = feed_one_frame(run->vmaf, fx);
    if (msg)
        return msg;
    mu_assert("vmaf_read_pictures(EOS) failed", !vmaf_read_pictures(run->vmaf, NULL, NULL, 0));
    for (size_t k = 0; k < count; k++) {
        if (vmaf_feature_score_at_index(run->vmaf, keys[k], &out[k], 0u)) {
            (void)fprintf(stderr, "\nmissing feature-name key: %s (%s twin)\n", keys[k],
                          run->use_vulkan ? "Vulkan" : "CPU");
            return "ADM feature-name key not emitted";
        }
    }
    return NULL;
}

static char *adm_run_close(AdmRun *run)
{
    if (run->vmaf)
        mu_assert("vmaf_close failed", !vmaf_close(run->vmaf));
    if (run->vk_state)
        vmaf_vulkan_state_free(&run->vk_state);
    return NULL;
}

/* Score one frame on the CPU or Vulkan twin and read `keys` into `out`.
 * Without a Vulkan device the Vulkan leg is skipped and `out` stays NaN. */
static char *run_adm(bool use_vulkan, const Fixture *fx, VmafFeatureDictionary *opts,
                     const char *const *keys, size_t count, double *out)
{
    for (size_t k = 0; k < count; k++)
        out[k] = NAN;
    AdmRun run = {.use_vulkan = use_vulkan};
    int skipped = 0;
    char *msg = adm_run_open(&run, opts, &skipped);
    if (!msg && !skipped)
        msg = adm_run_score(&run, fx, keys, count, out);
    char *close_msg = adm_run_close(&run);
    return msg ? msg : close_msg;
}

/* Every key of the case equal on the CPU and the Vulkan twin, bit for bit;
 * a skipped Vulkan leg passes. */
static char *compare_runs(const char *what, const Fixture *fx, const char *const *keys,
                          size_t count, const double *cpu, const double *vk)
{
    if (isnan(vk[0]))
        return NULL;
    unsigned mismatches = 0u;
    for (size_t k = 0; k < count; k++) {
        mu_assert("CPU adm output is non-finite", isfinite(cpu[k]));
        if (cpu[k] == vk[k])
            continue;
        mismatches++;
        (void)fprintf(stderr, "\n%s %ux%u %u-bit %s: cpu=%.17g vulkan=%.17g delta=%.3e\n", what,
                      fx->w, fx->h, fx->bpc, keys[k], cpu[k], vk[k], fabs(cpu[k] - vk[k]));
    }
    mu_assert("adm_vulkan differs from the CPU extractor", mismatches == 0u);
    return NULL;
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

/* The five scores plus, with `debug=true`, the frame ratio and the sums it
 * is formed from. The Vulkan twin does not provide `aim` / `adm3` (draft
 * scope), so those keys stay out of every list here. `suffix` is the
 * feature-name suffix of the option set. */
#define ADM_SCORE_KEYS(suffix)                                                                     \
    "integer_adm2" suffix, "integer_adm_scale0" suffix, "integer_adm_scale1" suffix,               \
        "integer_adm_scale2" suffix, "integer_adm_scale3" suffix
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
    "integer_adm_scale0",
    "integer_adm_scale1",
    "integer_adm_scale2",
    "integer_adm_scale3",
    ADM_DEBUG_KEYS(""),
};
#define NUM_DEBUG_KEYS (sizeof(DEBUG_KEYS) / sizeof(DEBUG_KEYS[0]))

/* The default model `vmaf_v1.0.16_3d0h` asks integer ADM for
 * VMAF_integer_feature_adm3_score with exactly these options. The
 * feature-name key both twins must emit is the fully-suffixed form below:
 * a twin whose option table is missing one entry emits a shorter key and
 * the comparison misses. */
static VmafFeatureDictionary *model_opts(void)
{
    static const char *const pairs[][2] = {
        {"adm_csf_mode", "2"},  {"adm_dlm_weight", "0.7"},    {"adm_enhn_gain_limit", "1.0"},
        {"adm_min_val", "0.5"}, {"adm_noise_weight", "0.02"}, {"adm_p_norm", "2.0"},
    };
    return opts_from(pairs, sizeof(pairs) / sizeof(pairs[0]));
}


static char *check_exact(const char *what, const Fixture *fx,
                         VmafFeatureDictionary *(*make_opts)(void), const char *const *keys,
                         size_t count)
{
    double cpu[MAX_KEYS];
    double vk[MAX_KEYS];
    mu_assert("check_exact: too many keys", count <= MAX_KEYS);
    char *msg = run_adm(false, fx, make_opts ? make_opts() : NULL, keys, count, cpu);
    if (!msg)
        msg = run_adm(true, fx, make_opts ? make_opts() : NULL, keys, count, vk);
    return msg ? msg : compare_runs(what, fx, keys, count, cpu, vk);
}

/* The exact cases, one per fixture; the debug case carries the per-scale
 * sums (the strongest bit-exactness witness). The non-default-option and
 * 10-bit drafts are refusal cases: the twin honours the default
 * configuration only (see #3319 Known follow-ups). */
static char *test_adm_vulkan_textured_debug_exact(void)
{
    const Fixture fx = {FIXTURE_W, FIXTURE_H, 8u, false};
    return check_exact("adm vulkan textured debug", &fx, debug_opts, DEBUG_KEYS, NUM_DEBUG_KEYS);
}

/* The file fixtures: the natural-content yuv420p8 pairs the SYCL and
 * no-reference oracles already share (testdata/ref_576x324_48f.yuv and
 * ref_640x480_48f.yuv), 48 frames each. The synthetic fixtures above cover
 * two single frames at 256x144; these two cases run both twins across real
 * content at larger geometries, where the per-scale accumulators carry
 * magnitudes the tiny frames never reach. The closed duplicate draft's
 * committed evidence (#3333, now superseded by this PR) produced CPU
 * references on exactly these fixtures and noted that both CPU anchors agree
 * on them, so this gate is the geometry/content witness, not an
 * own-tap-wrap discriminator. */

#define FILE_FRAMES 48u

typedef struct FileFixture {
    unsigned w;
    unsigned h;
    char ref[512];
    char dis[512];
} FileFixture;

/* Read one plane of an 8-bit yuv420p frame straight into a picture,
 * honouring the picture stride. */
static int read_plane(FILE *f, VmafPicture *pic, unsigned plane)
{
    uint8_t *base = (uint8_t *)pic->data[plane];
    for (unsigned row = 0; row < pic->h[plane]; row++) {
        if (fread(base + (size_t)row * pic->stride[plane], 1u, pic->w[plane], f) !=
            pic->w[plane])
            return -1;
    }
    return 0;
}

static char *read_file_frame(FILE *f, const FileFixture *fx, VmafPicture *pic)
{
    mu_assert("picture alloc failed",
              !vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8u, fx->w, fx->h));
    for (unsigned p = 0; p < 3u; p++)
        mu_assert("fixture truncated", !read_plane(f, pic, p));
    return NULL;
}

/* Score `FILE_FRAMES` frames of the file fixture on one anchor and read every
 * key of every frame into `out` ([frame][key], indexed scores). One context
 * and one feature open for the whole run, exactly how the CLI consumes the
 * same files. */
static char *run_adm_file(bool use_vulkan, const FileFixture *fx,
                          VmafFeatureDictionary *opts, const char *const *keys,
                          size_t count, double out[FILE_FRAMES][MAX_KEYS])
{
    for (unsigned i = 0; i < FILE_FRAMES; i++)
        for (size_t k = 0; k < count; k++)
            out[i][k] = NAN;
    AdmRun run = {.use_vulkan = use_vulkan};
    int skipped = 0;
    char *msg = adm_run_open(&run, opts, &skipped);
    if (msg || skipped)
        return msg;
    FILE *fref = fopen(fx->ref, "rb");
    FILE *fdis = fopen(fx->dis, "rb");
    mu_assert("fixture yuv missing", fref && fdis);
    for (unsigned i = 0; i < FILE_FRAMES && !msg; i++) {
        VmafPicture ref;
        VmafPicture dist;
        msg = read_file_frame(fref, fx, &ref);
        if (!msg)
            msg = read_file_frame(fdis, fx, &dist);
        if (!msg) {
            const int err = vmaf_read_pictures(run.vmaf, &ref, &dist, i);
            if (err) {
                (void)fprintf(stderr, "\n%s: vmaf_read_pictures frame %u err=%d\n",
                              use_vulkan ? "vulkan" : "cpu", i, err);
                msg = "vmaf_read_pictures failed";
            }
        }
    }
    if (fref)
        fclose(fref);
    if (fdis)
        fclose(fdis);
    if (!msg) {
        const int err = vmaf_read_pictures(run.vmaf, NULL, NULL, 0);
        if (err) {
            (void)fprintf(stderr, "\n%s: vmaf_read_pictures(EOS) err=%d\n",
                          use_vulkan ? "vulkan" : "cpu", err);
            msg = "vmaf_read_pictures(EOS) failed";
        }
    }
    for (unsigned i = 0; i < FILE_FRAMES && !msg; i++) {
        for (size_t k = 0; k < count; k++) {
            if (vmaf_feature_score_at_index(run.vmaf, keys[k], &out[i][k], i)) {
                (void)fprintf(stderr, "\nmissing feature-name key: %s frame %u (%s twin)\n",
                              keys[k], i, use_vulkan ? "Vulkan" : "CPU");
                msg = "ADM feature-name key not emitted";
                break;
            }
        }
    }
    char *close_msg = adm_run_close(&run);
    return msg ? msg : close_msg;
}

static char *check_file_exact(const char *what, const FileFixture *fx)
{
    double cpu[FILE_FRAMES][MAX_KEYS];
    double vk[FILE_FRAMES][MAX_KEYS];
    char *msg = run_adm_file(false, fx, debug_opts(), DEBUG_KEYS, NUM_DEBUG_KEYS, cpu);
    if (!msg)
        msg = run_adm_file(true, fx, debug_opts(), DEBUG_KEYS, NUM_DEBUG_KEYS, vk);
    if (msg)
        return msg;
    if (isnan(vk[0][0]))
        return NULL; /* skipped: no Vulkan device */
    for (unsigned i = 0; i < FILE_FRAMES; i++) {
        unsigned mismatches = 0u;
        for (size_t k = 0; k < NUM_DEBUG_KEYS; k++) {
            mu_assert("CPU adm output is non-finite", isfinite(cpu[i][k]));
            if (cpu[i][k] == vk[i][k])
                continue;
            mismatches++;
            (void)fprintf(stderr,
                          "\n%s frame %u %s: cpu=%.17g vulkan=%.17g delta=%.3e\n", what, i,
                          DEBUG_KEYS[k], cpu[i][k], vk[i][k], fabs(cpu[i][k] - vk[i][k]));
        }
        mu_assert("adm_vulkan differs from the CPU extractor on the file fixture",
                  mismatches == 0u);
    }
    return NULL;
}

static char *test_adm_vulkan_fixture576_debug_exact(void)
{
    const FileFixture fx = {576u, 324u,
                            VULKAN_ADM_TESTDATA_DIR "/ref_576x324_48f.yuv",
                            VULKAN_ADM_TESTDATA_DIR "/dis_576x324_48f.yuv"};
    return check_file_exact("adm vulkan fixture 576x324", &fx);
}

static char *test_adm_vulkan_fixture640_debug_exact(void)
{
    const FileFixture fx = {640u, 480u,
                            VULKAN_ADM_TESTDATA_DIR "/ref_640x480_48f.yuv",
                            VULKAN_ADM_TESTDATA_DIR "/dis_640x480_48f.yuv"};
    return check_file_exact("adm vulkan fixture 640x480", &fx);
}

static char *test_adm_vulkan_sparse_debug_exact(void)
{
    const Fixture fx = {FIXTURE_W, FIXTURE_H, 8u, true};
    return check_exact("adm vulkan sparse debug", &fx, debug_opts, DEBUG_KEYS, NUM_DEBUG_KEYS);
}

/* The draft twin is 8-bit 4:2:0 only (a 10-bit port is a #3319 known
 * follow-up); a 10-bit frame must be refused cleanly instead of scoring:
 * the registration succeeds but the frame read fails. */
static char *test_adm_vulkan_10bit_refused(void)
{
    AdmRun run = {.use_vulkan = true};
    int skipped = 0;
    char *msg = adm_run_init(&run, &skipped);
    if (msg || skipped)
        return msg;
    mu_assert("10-bit adm_vulkan registration must succeed",
              !vmaf_use_feature(run.vmaf, "adm_vulkan", debug_opts()));
    const Fixture fx = {FIXTURE_W, FIXTURE_H, 10u, false};
    VmafPicture ref;
    VmafPicture dist;
    mu_assert("fill 10-bit reference failed", !fill_picture(&ref, &fx, false));
    mu_assert("fill 10-bit distorted failed", !fill_picture(&dist, &fx, true));
    const int err = vmaf_read_pictures(run.vmaf, &ref, &dist, 0u);
    vmaf_close(run.vmaf);
    vmaf_vulkan_state_free(&run.vk_state);
    mu_assert("10-bit adm_vulkan frame must be refused", err != 0);
    return NULL;
}

/* The draft twin honours the default configuration only; a non-default
 * option set must be refused cleanly (the CPU twin scores it — the
 * options plumbed through check_default_configuration are a #3319 known
 * follow-up). Assert the refusal instead of comparing scores. */
static char *test_adm_vulkan_model_options_refused(void)
{
    AdmRun run = {.use_vulkan = true};
    int skipped = 0;
    char *msg = adm_run_init(&run, &skipped);
    if (msg || skipped)
        return msg;
    mu_assert("model-option adm_vulkan registration must succeed",
              !vmaf_use_feature(run.vmaf, "adm_vulkan", model_opts()));
    const Fixture fx = {FIXTURE_W, FIXTURE_H, 8u, false};
    VmafPicture ref;
    VmafPicture dist;
    mu_assert("fill model-option reference failed", !fill_picture(&ref, &fx, false));
    mu_assert("fill model-option distorted failed", !fill_picture(&dist, &fx, true));
    const int err = vmaf_read_pictures(run.vmaf, &ref, &dist, 0u);
    vmaf_close(run.vmaf);
    vmaf_vulkan_state_free(&run.vk_state);
    mu_assert("model-option adm_vulkan frame must be refused", err != 0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_adm_vulkan_textured_debug_exact);
    mu_run_test(test_adm_vulkan_sparse_debug_exact);
    mu_run_test(test_adm_vulkan_fixture576_debug_exact);
    mu_run_test(test_adm_vulkan_fixture640_debug_exact);
    mu_run_test(test_adm_vulkan_10bit_refused);
    mu_run_test(test_adm_vulkan_model_options_refused);
    return NULL;
}
