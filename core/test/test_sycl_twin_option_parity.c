/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * CPU vs SYCL parity for the CPU options the SYCL twins used to lack
 * (T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26, SYCL part):
 *
 *   psnr_sycl          enable_mse, enable_apsnr, reduced_hbd_peak, min_sse
 *   integer_ssim_sycl  enable_db, clip_db
 *   float_ssim_sycl    enable_lcs, enable_db, clip_db
 *   float_motion_sycl  motion_max_val (and motion_force_zero, which it
 *                      declared but did not honour, and motion_fps_weight
 *                      on the debug `motion` score)
 *
 * Positive: each option set on both sides gives the CPU score, per frame --
 * bit-exact for PSNR (integer SSE, same host arithmetic via psnr_score.h),
 * within the twin's existing parity tolerance otherwise. Negative: an
 * option left at its default adds no output, and an unknown key or an
 * out-of-range value is refused by both sides. Boundary: identical frames
 * (SSE == 0, perfect SSIM) report the same sentinel / +inf / clip_db ceiling
 * as the CPU; motion_max_val = 0 zeroes every score; odd 4:2:0 and 10-bit
 * 4:2:2 geometry.
 *
 * The option-table checks need no device. The parity checks exit 77 (skip)
 * when no SYCL device is visible.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "dict.h"
#include "feature/feature_extractor.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#define TOL_EXACT 0.0
/* Existing twin contracts: test_sycl_ssim_parity.c (1e-4),
 * test_sycl_float_ssim_parity.c (5e-4, Research-0985),
 * test_sycl_float_motion_parity.c (1e-4). */
#define TOL_SSIM 1e-4
#define TOL_FLOAT_SSIM 5e-4
#define TOL_MOTION 1e-4
#define NAME_LEN 64u
#define MAX_FRAMES 8u

typedef struct Fixture {
    enum VmafPixelFormat pix_fmt;
    unsigned bpc;
    unsigned w;
    unsigned h;
    unsigned frames;
    bool identical;
} Fixture;

/* Odd 4:2:0 (ceil chroma), odd-width 10-bit 4:2:2, and an identical pair. */
static const Fixture FX_ODD8 = {VMAF_PIX_FMT_YUV420P, 8u, 161u, 91u, 4u, false};
static const Fixture FX_ODD10 = {VMAF_PIX_FMT_YUV422P, 10u, 129u, 67u, 4u, false};
static const Fixture FX_SAME8 = {VMAF_PIX_FMT_YUV420P, 8u, 161u, 91u, 3u, true};
static const Fixture FX_MOTION = {VMAF_PIX_FMT_YUV420P, 8u, 161u, 91u, 6u, false};

typedef struct Pair {
    VmafContext *cpu;
    VmafContext *gpu;
    VmafSyclState *sycl;
} Pair;

/* ------------------------------------------------------------------ */
/* Fixture generation                                                 */
/* ------------------------------------------------------------------ */

static unsigned sample_at(unsigned x, unsigned y, unsigned frame, unsigned plane, unsigned bpc)
{
    /* Quadratic shift makes the frame-to-frame motion grow over time. */
    const unsigned xs = x + frame * frame * 2u;
    const unsigned base = ((xs * 7u + y * 13u + plane * 31u) ^ ((xs * y) >> 3)) & 0xFFu;
    if (bpc == 8u)
        return base;
    return (base << (bpc - 8u)) | (xs & ((1u << (bpc - 8u)) - 1u));
}

static int noise_at(unsigned x, unsigned y, unsigned frame)
{
    const uint32_t h = (x * 73856093u) ^ (y * 19349663u) ^ (frame * 83492791u);
    return (int)(((h * 1103515245u + 12345u) >> 16) & 7u) - 3;
}

static unsigned distort(unsigned value, unsigned x, unsigned y, unsigned frame, unsigned bpc)
{
    const int max = (1 << bpc) - 1;
    const int shifted = (int)value + noise_at(x, y, frame) * (1 << (bpc - 8u));
    return (unsigned)(shifted < 0 ? 0 : (shifted > max ? max : shifted));
}

static void fill_plane(VmafPicture *pic, unsigned plane, unsigned frame, bool distorted)
{
    for (unsigned y = 0; y < pic->h[plane]; y++) {
        for (unsigned x = 0; x < pic->w[plane]; x++) {
            unsigned v = sample_at(x, y, frame, plane, pic->bpc);
            if (distorted)
                v = distort(v, x, y, frame, pic->bpc);
            if (pic->bpc == 8u) {
                uint8_t *row = (uint8_t *)pic->data[plane] + (size_t)y * pic->stride[plane];
                row[x] = (uint8_t)v;
            } else {
                uint16_t *row =
                    (uint16_t *)((uint8_t *)pic->data[plane] + (size_t)y * pic->stride[plane]);
                row[x] = (uint16_t)v;
            }
        }
    }
}

static int make_picture(const Fixture *fx, unsigned frame, bool distorted, VmafPicture *pic)
{
    const int err = vmaf_picture_alloc(pic, fx->pix_fmt, fx->bpc, fx->w, fx->h);
    if (err)
        return err;
    for (unsigned p = 0; p < 3u; p++)
        fill_plane(pic, p, frame, distorted);
    return 0;
}

static int feed(VmafContext *vmaf, const Fixture *fx)
{
    for (unsigned i = 0; i < fx->frames; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = make_picture(fx, i, false, &ref);
        if (err)
            return err;
        err = make_picture(fx, i, !fx->identical, &dist);
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

/* ------------------------------------------------------------------ */
/* Running one feature on the CPU and on the SYCL twin                */
/* ------------------------------------------------------------------ */

static int use_feature(VmafContext *vmaf, const char *name, const char *const *opts)
{
    VmafFeatureDictionary *dict = NULL;
    for (unsigned i = 0; opts && opts[i]; i += 2u) {
        const int err = vmaf_feature_dictionary_set(&dict, opts[i], opts[i + 1u]);
        if (err) {
            (void)vmaf_feature_dictionary_free(&dict);
            return err;
        }
    }
    return vmaf_use_feature(vmaf, name, dict);
}

static void pair_close(Pair *pair)
{
    if (pair->cpu)
        (void)vmaf_close(pair->cpu);
    if (pair->gpu)
        (void)vmaf_close(pair->gpu);
    if (pair->sycl)
        vmaf_sycl_state_free(&pair->sycl);
    pair->cpu = NULL;
    pair->gpu = NULL;
    pair->sycl = NULL;
}

/* Returns true when a SYCL device is available and the twin context is
 * ready; false means "skip" (no device). */
static bool open_gpu(Pair *pair)
{
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    if (vmaf_sycl_state_init(&pair->sycl, sycl_cfg) != 0 || !pair->sycl)
        return false;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    if (vmaf_init(&pair->gpu, cfg))
        return false;
    return vmaf_sycl_import_state(pair->gpu, pair->sycl) == 0;
}

/* Run `cpu_name` and `twin` with the same options over `fx`. Sets
 * mu_skipped and returns NULL with *ran == false when no device exists. */
static mu_message_t pair_run(Pair *pair, const Fixture *fx, const char *cpu_name, const char *twin,
                             const char *const *opts, bool *ran)
{
    *ran = false;
    memset(pair, 0, sizeof(*pair));
    if (!open_gpu(pair)) {
        pair_close(pair);
        mu_skipped = 1;
        return NULL;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    mu_assert("CPU vmaf_init failed", !vmaf_init(&pair->cpu, cfg));
    mu_assert("CPU extractor rejected the options", !use_feature(pair->cpu, cpu_name, opts));
    mu_assert("SYCL twin rejected the options", !use_feature(pair->gpu, twin, opts));
    mu_assert("CPU run failed", !feed(pair->cpu, fx));
    mu_assert("SYCL run failed", !feed(pair->gpu, fx));
    *ran = true;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Comparisons                                                        */
/* ------------------------------------------------------------------ */

static bool within(double cpu, double gpu, double tol)
{
    return cpu == gpu || fabs(cpu - gpu) <= tol;
}

/* `tol` is a linear-domain tolerance; `db` maps it through the slope of
 * -10*log10(1 - x), which is 10 / ln(10) * 10^(dB / 10). */
static mu_message_t expect_close(const Pair *pair, const char *name, unsigned frames, double tol,
                                 bool db)
{
    for (unsigned i = 0; i < frames; i++) {
        double cpu = NAN;
        double gpu = NAN;
        mu_assert("feature missing on the CPU",
                  !vmaf_feature_score_at_index(pair->cpu, name, &cpu, i));
        mu_assert("feature missing on the SYCL twin",
                  !vmaf_feature_score_at_index(pair->gpu, name, &gpu, i));
        const double limit =
            db && isfinite(cpu) ? tol * (10.0 / log(10.0)) * pow(10.0, cpu / 10.0) : tol;
        if (!within(cpu, gpu, limit)) {
            (void)fprintf(stderr, "\n%s[%u]: cpu=%.17g sycl=%.17g limit=%.3g\n", name, i, cpu, gpu,
                          limit);
            return "SYCL twin differs from the CPU beyond the twin's parity tolerance";
        }
    }
    return NULL;
}

static bool feature_present(VmafContext *vmaf, const char *name)
{
    double score = 0.0;
    return vmaf_feature_score_at_index(vmaf, name, &score, 0u) == 0;
}

static mu_message_t expect_absent(const Pair *pair, const char *name)
{
    mu_assert("CPU emitted a feature its option leaves off", !feature_present(pair->cpu, name));
    mu_assert("SYCL twin emitted a feature its option leaves off",
              !feature_present(pair->gpu, name));
    return NULL;
}

static mu_message_t expect_all(const Pair *pair, const char *name, unsigned frames, double value)
{
    for (unsigned i = 0; i < frames; i++) {
        double cpu = NAN;
        double gpu = NAN;
        mu_assert("feature missing on the CPU",
                  !vmaf_feature_score_at_index(pair->cpu, name, &cpu, i));
        mu_assert("feature missing on the SYCL twin",
                  !vmaf_feature_score_at_index(pair->gpu, name, &gpu, i));
        mu_assert("CPU value differs from the expected boundary value", cpu == value);
        mu_assert("SYCL value differs from the expected boundary value", gpu == value);
    }
    return NULL;
}

/* Aggregates have no public getter: read them back from the JSON output at
 * round-trip precision. Returns 0 and sets *value, or -ENOENT. */
static int read_aggregate(VmafContext *vmaf, const char *path, const char *name, double *value)
{
    if (vmaf_write_output_with_format(vmaf, path, VMAF_OUTPUT_FORMAT_JSON, "%.17g"))
        return -EIO;
    FILE *fh = fopen(path, "rb");
    if (!fh)
        return -EIO;
    static char buf[1u << 16];
    const size_t n = fread(buf, 1u, sizeof(buf) - 1u, fh);
    (void)fclose(fh);
    (void)remove(path);
    buf[n] = '\0';
    char key[NAME_LEN + 8u];
    (void)snprintf(key, sizeof(key), "\"%s\": ", name);
    const char *section = strstr(buf, "\"aggregate_metrics\"");
    const char *at = section ? strstr(section, key) : NULL;
    if (!at)
        return -ENOENT;
    char *end = NULL;
    *value = strtod(at + strlen(key), &end);
    return end == at + strlen(key) ? -ENOENT : 0;
}

static mu_message_t expect_aggregate(const Pair *pair, const char *name)
{
    double cpu = NAN;
    double gpu = NAN;
    mu_assert("CPU aggregate missing",
              !read_aggregate(pair->cpu, "twin_option_parity_cpu.json", name, &cpu));
    mu_assert("SYCL aggregate missing",
              !read_aggregate(pair->gpu, "twin_option_parity_sycl.json", name, &gpu));
    if (cpu != gpu) {
        (void)fprintf(stderr, "\n%s: cpu=%.17g sycl=%.17g\n", name, cpu, gpu);
        return "SYCL aggregate differs from the CPU";
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Device-free: option tables                                         */
/* ------------------------------------------------------------------ */

typedef struct OptionCase {
    const char *twin;
    const char *cpu;
    const char *key;
    const char *value;
} OptionCase;

static const OptionCase OPTION_CASES[] = {
    {"psnr_sycl", "psnr", "enable_mse", "true"},
    {"psnr_sycl", "psnr", "enable_apsnr", "true"},
    {"psnr_sycl", "psnr", "reduced_hbd_peak", "true"},
    {"psnr_sycl", "psnr", "min_sse", "0.5"},
    {"integer_ssim_sycl", "ssim", "enable_db", "true"},
    {"integer_ssim_sycl", "ssim", "clip_db", "true"},
    {"float_ssim_sycl", "float_ssim", "enable_lcs", "true"},
    {"float_ssim_sycl", "float_ssim", "enable_db", "true"},
    {"float_ssim_sycl", "float_ssim", "clip_db", "true"},
    {"float_motion_sycl", "float_motion", "motion_max_val", "2.5"},
    {"float_motion_sycl", "float_motion", "mmxv", "2.5"},
};

static const VmafOption *lookup_option(const VmafFeatureExtractor *fex, const char *key)
{
    for (unsigned i = 0; fex && fex->options && fex->options[i].name; i++) {
        const VmafOption *opt = &fex->options[i];
        if (!strcmp(opt->name, key) || (opt->alias && !strcmp(opt->alias, key)))
            return opt;
    }
    return NULL;
}

static bool same_option(const VmafOption *a, const VmafOption *b)
{
    if (strcmp(a->name, b->name) != 0 || a->type != b->type || a->flags != b->flags)
        return false;
    if ((a->alias == NULL) != (b->alias == NULL) || (a->alias && strcmp(a->alias, b->alias) != 0))
        return false;
    if (a->type == VMAF_OPT_TYPE_BOOL)
        return a->default_val.b == b->default_val.b;
    return a->default_val.d == b->default_val.d && a->min == b->min && a->max == b->max;
}

/* ADR-1183 gate with a one-entry dictionary. The gate points the rejected
 * key into the dictionary, so it is copied to `unsupported` (empty when the
 * gate passes) before the dictionary is freed. */
static bool honours(const VmafFeatureExtractor *fex, const char *key, const char *value,
                    char unsupported[NAME_LEN])
{
    VmafDictionary *dict = NULL;
    unsupported[0] = '\0';
    if (vmaf_dictionary_set(&dict, key, value, 0))
        return false;
    const char *rejected = NULL;
    const bool ok = vmaf_feature_extractor_honours_options(fex, dict, &rejected);
    if (rejected)
        (void)snprintf(unsupported, NAME_LEN, "%s", rejected);
    (void)vmaf_dictionary_free(&dict);
    return ok;
}

static char *test_twin_option_tables_match_cpu(void)
{
    for (size_t i = 0; i < sizeof(OPTION_CASES) / sizeof(OPTION_CASES[0]); i++) {
        const OptionCase *c = &OPTION_CASES[i];
        const VmafFeatureExtractor *twin = vmaf_get_feature_extractor_by_name(c->twin);
        const VmafFeatureExtractor *cpu = vmaf_get_feature_extractor_by_name(c->cpu);
        mu_assert("extractor not registered", twin && cpu);
        const VmafOption *twin_opt = lookup_option(twin, c->key);
        const VmafOption *cpu_opt = lookup_option(cpu, c->key);
        mu_assert("SYCL twin does not declare the CPU option", twin_opt && cpu_opt);
        mu_assert("twin option differs from the CPU declaration", same_option(twin_opt, cpu_opt));
        char unsupported[NAME_LEN];
        mu_assert("ADR-1183 gate still keeps the option off the twin",
                  honours(twin, c->key, c->value, unsupported) && !unsupported[0]);
    }
    return NULL;
}

static char *test_twin_rejects_unknown_option(void)
{
    const char *const twins[] = {"psnr_sycl", "integer_ssim_sycl", "float_ssim_sycl",
                                 "float_motion_sycl"};
    for (size_t i = 0; i < sizeof(twins) / sizeof(twins[0]); i++) {
        const VmafFeatureExtractor *twin = vmaf_get_feature_extractor_by_name(twins[i]);
        char unsupported[NAME_LEN];
        mu_assert("twin not registered", twin != NULL);
        mu_assert("an unknown key must keep the feature off the twin",
                  !honours(twin, "enable_frobnication", "true", unsupported));
        mu_assert("the gate must name the unknown key",
                  !strcmp(unsupported, "enable_frobnication"));
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* psnr_sycl                                                           */
/* ------------------------------------------------------------------ */

static mu_message_t psnr_all_options(const Fixture *fx)
{
    static const char *const opts[] = {
        "enable_mse", "true",    "enable_apsnr", "true", "reduced_hbd_peak",
        "true",       "min_sse", "0.5",          NULL};
    static const char *const names[] = {"psnr_y", "psnr_cb", "psnr_cr",
                                        "mse_y",  "mse_cb",  "mse_cr"};
    static const char *const aggregates[] = {"apsnr_y", "apsnr_cb", "apsnr_cr"};
    Pair pair;
    bool ran = false;
    mu_assert_msg(pair_run(&pair, fx, "psnr", "psnr_sycl", opts, &ran));
    mu_message_t msg = NULL;
    for (size_t i = 0; ran && !msg && i < sizeof(names) / sizeof(names[0]); i++)
        msg = expect_close(&pair, names[i], fx->frames, TOL_EXACT, false);
    for (size_t i = 0; ran && !msg && i < sizeof(aggregates) / sizeof(aggregates[0]); i++)
        msg = expect_aggregate(&pair, aggregates[i]);
    pair_close(&pair);
    return msg;
}

static char *test_psnr_options_bit_exact(void)
{
    mu_assert_msg(psnr_all_options(&FX_ODD8));
    mu_assert_msg(psnr_all_options(&FX_ODD10));
    return NULL;
}

/* Identical frames: SSE == 0 on every plane, so psnr_y and apsnr_y both
 * report the min_sse ceiling ceil(10 * log10(255^2 / (0.5 / (w * h)))). */
static char *test_psnr_min_sse_identical_frames(void)
{
    static const char *const opts[] = {"min_sse", "0.5", "enable_apsnr", "true", NULL};
    const double ceiling = ceil(10.0 * log10(255.0 * 255.0 / (0.5 / (161.0 * 91.0))));
    Pair pair;
    bool ran = false;
    mu_assert_msg(pair_run(&pair, &FX_SAME8, "psnr", "psnr_sycl", opts, &ran));
    mu_message_t msg = ran ? expect_all(&pair, "psnr_y", FX_SAME8.frames, ceiling) : NULL;
    if (ran && !msg)
        msg = expect_aggregate(&pair, "apsnr_y");
    pair_close(&pair);
    return msg;
}

static char *test_psnr_defaults_add_no_outputs(void)
{
    Pair pair;
    bool ran = false;
    mu_assert_msg(pair_run(&pair, &FX_ODD8, "psnr", "psnr_sycl", NULL, &ran));
    mu_message_t msg = ran ? expect_absent(&pair, "mse_y") : NULL;
    if (ran && !msg)
        msg = expect_close(&pair, "psnr_y", FX_ODD8.frames, TOL_EXACT, false);
    pair_close(&pair);
    return msg;
}

/* ------------------------------------------------------------------ */
/* integer_ssim_sycl / float_ssim_sycl                                 */
/* ------------------------------------------------------------------ */

static mu_message_t ssim_db_case(const Fixture *fx, const char *cpu_name, const char *twin,
                                 const char *feature, double tol)
{
    static const char *const opts[] = {"enable_db", "true", "clip_db", "true", NULL};
    Pair pair;
    bool ran = false;
    mu_assert_msg(pair_run(&pair, fx, cpu_name, twin, opts, &ran));
    mu_message_t msg = ran ? expect_close(&pair, feature, fx->frames, tol, true) : NULL;
    pair_close(&pair);
    return msg;
}

static char *test_integer_ssim_db_options(void)
{
    mu_assert_msg(ssim_db_case(&FX_ODD8, "ssim", "integer_ssim_sycl", "ssim", TOL_SSIM));
    mu_assert_msg(ssim_db_case(&FX_ODD10, "ssim", "integer_ssim_sycl", "ssim", TOL_SSIM));
    /* Identical frames: both sides must hit the clip_db ceiling exactly. */
    mu_assert_msg(ssim_db_case(&FX_SAME8, "ssim", "integer_ssim_sycl", "ssim", TOL_EXACT));
    return NULL;
}

static char *test_float_ssim_db_options(void)
{
    mu_assert_msg(
        ssim_db_case(&FX_ODD8, "float_ssim", "float_ssim_sycl", "float_ssim", TOL_FLOAT_SSIM));
    mu_assert_msg(
        ssim_db_case(&FX_ODD10, "float_ssim", "float_ssim_sycl", "float_ssim", TOL_FLOAT_SSIM));
    mu_assert_msg(
        ssim_db_case(&FX_SAME8, "float_ssim", "float_ssim_sycl", "float_ssim", TOL_EXACT));
    return NULL;
}

/* enable_db without clip_db: a perfect score is +inf on both sides
 * (ADR-1221), not the finite dB of an fp32 rounding residue. */
static mu_message_t ssim_unclipped_perfect(const char *cpu_name, const char *twin,
                                           const char *feature)
{
    static const char *const opts[] = {"enable_db", "true", NULL};
    Pair pair;
    bool ran = false;
    mu_assert_msg(pair_run(&pair, &FX_SAME8, cpu_name, twin, opts, &ran));
    mu_message_t msg = ran ? expect_all(&pair, feature, FX_SAME8.frames, INFINITY) : NULL;
    pair_close(&pair);
    return msg;
}

static char *test_ssim_db_identical_frames_unclipped(void)
{
    mu_assert_msg(ssim_unclipped_perfect("ssim", "integer_ssim_sycl", "ssim"));
    mu_assert_msg(ssim_unclipped_perfect("float_ssim", "float_ssim_sycl", "float_ssim"));
    return NULL;
}

static mu_message_t float_ssim_lcs_case(const Fixture *fx)
{
    static const char *const opts[] = {"enable_lcs", "true", NULL};
    static const char *const names[] = {"float_ssim", "float_ssim_l", "float_ssim_c",
                                        "float_ssim_s"};
    Pair pair;
    bool ran = false;
    mu_assert_msg(pair_run(&pair, fx, "float_ssim", "float_ssim_sycl", opts, &ran));
    mu_message_t msg = NULL;
    for (size_t i = 0; ran && !msg && i < sizeof(names) / sizeof(names[0]); i++)
        msg = expect_close(&pair, names[i], fx->frames, TOL_FLOAT_SSIM, false);
    pair_close(&pair);
    return msg;
}

static char *test_float_ssim_lcs(void)
{
    mu_assert_msg(float_ssim_lcs_case(&FX_ODD8));
    mu_assert_msg(float_ssim_lcs_case(&FX_ODD10));
    mu_assert_msg(float_ssim_lcs_case(&FX_SAME8));
    Pair pair;
    bool ran = false;
    mu_assert_msg(pair_run(&pair, &FX_ODD8, "float_ssim", "float_ssim_sycl", NULL, &ran));
    mu_message_t msg = ran ? expect_absent(&pair, "float_ssim_l") : NULL;
    pair_close(&pair);
    return msg;
}

/* ------------------------------------------------------------------ */
/* float_motion_sycl                                                   */
/* ------------------------------------------------------------------ */

static mu_message_t motion_case(const char *key, const char *value, const char *motion2,
                                const char *motion, double tol)
{
    const char *const opts[] = {key, value, NULL};
    Pair pair;
    bool ran = false;
    mu_assert_msg(pair_run(&pair, &FX_MOTION, "float_motion", "float_motion_sycl", opts, &ran));
    mu_message_t msg = ran ? expect_close(&pair, motion2, FX_MOTION.frames, tol, false) : NULL;
    if (ran && !msg)
        msg = expect_close(&pair, motion, FX_MOTION.frames, tol, false);
    pair_close(&pair);
    return msg;
}

/* Midpoint of the CPU default motion2 range, so the cap clips some frames
 * and leaves others. Returns a negative value on failure. */
static double motion_midpoint(void)
{
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    double lo = INFINITY;
    double hi = 0.0;
    if (vmaf_init(&vmaf, cfg) || use_feature(vmaf, "float_motion", NULL) || feed(vmaf, &FX_MOTION))
        return -1.0;
    for (unsigned i = 1; i < FX_MOTION.frames; i++) {
        double score = 0.0;
        if (vmaf_feature_score_at_index(vmaf, "VMAF_feature_motion2_score", &score, i))
            return -1.0;
        lo = score < lo ? score : lo;
        hi = score > hi ? score : hi;
    }
    (void)vmaf_close(vmaf);
    return hi > lo ? floor((lo + hi) * 5.0) / 10.0 : -1.0;
}

static char *test_float_motion_max_val(void)
{
    const double cap = motion_midpoint();
    mu_assert("fixture motion does not span a clip point", cap > 0.0);
    char value[NAME_LEN];
    char motion2[NAME_LEN];
    char motion[NAME_LEN];
    (void)snprintf(value, sizeof(value), "%.17g", cap);
    (void)snprintf(motion2, sizeof(motion2), "motion2_mmxv_%g", cap);
    (void)snprintf(motion, sizeof(motion), "motion_mmxv_%g", cap);
    mu_assert_msg(motion_case("motion_max_val", value, motion2, motion, TOL_MOTION));
    /* Boundary: a zero cap clips every score to exactly zero. */
    mu_assert_msg(motion_case("motion_max_val", "0", "motion2_mmxv_0", "motion_mmxv_0", TOL_EXACT));
    return NULL;
}

/* The debug `motion` score is motion_clip()ped like `motion2`, so it carries
 * motion_fps_weight too (the twin used to emit it unweighted). */
static char *test_float_motion_fps_weight_debug_score(void)
{
    return motion_case("motion_fps_weight", "2", "motion2_mfw_2", "motion_mfw_2", 2.0 * TOL_MOTION);
}

static char *test_float_motion_force_zero(void)
{
    static const char *const opts[] = {"motion_force_zero", "true", NULL};
    Pair pair;
    bool ran = false;
    mu_assert_msg(pair_run(&pair, &FX_MOTION, "float_motion", "float_motion_sycl", opts, &ran));
    mu_message_t msg = ran ? expect_all(&pair, "motion2_force_0", FX_MOTION.frames, 0.0) : NULL;
    if (ran && !msg)
        msg = expect_all(&pair, "motion_force_0", FX_MOTION.frames, 0.0);
    pair_close(&pair);
    return msg;
}

/* Negative: motion_max_val above the declared range is refused by both. */
static char *test_float_motion_max_val_out_of_range(void)
{
    static const char *const opts[] = {"motion_max_val", "10001", NULL};
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *cpu = NULL;
    Pair pair;
    memset(&pair, 0, sizeof(pair));
    if (!open_gpu(&pair)) {
        pair_close(&pair);
        mu_skipped = 1;
        return NULL;
    }
    mu_assert("CPU vmaf_init failed", !vmaf_init(&cpu, cfg));
    const int cpu_err = use_feature(cpu, "float_motion", opts);
    const int gpu_err = use_feature(pair.gpu, "float_motion_sycl", opts);
    (void)vmaf_close(cpu);
    pair_close(&pair);
    mu_assert("CPU accepted an out-of-range motion_max_val", cpu_err != 0);
    mu_assert("SYCL twin accepted an out-of-range motion_max_val", gpu_err != 0);
    return NULL;
}

static char *run_table_and_psnr_tests(void)
{
    mu_run_test(test_twin_option_tables_match_cpu);
    mu_run_test(test_twin_rejects_unknown_option);
    mu_run_test(test_psnr_options_bit_exact);
    mu_run_test(test_psnr_min_sse_identical_frames);
    mu_run_test(test_psnr_defaults_add_no_outputs);
    return NULL;
}

static char *run_ssim_tests(void)
{
    mu_run_test(test_integer_ssim_db_options);
    mu_run_test(test_float_ssim_db_options);
    mu_run_test(test_ssim_db_identical_frames_unclipped);
    mu_run_test(test_float_ssim_lcs);
    return NULL;
}

static char *run_motion_tests(void)
{
    mu_run_test(test_float_motion_max_val);
    mu_run_test(test_float_motion_fps_weight_debug_score);
    mu_run_test(test_float_motion_force_zero);
    mu_run_test(test_float_motion_max_val_out_of_range);
    return NULL;
}

char *run_tests(void)
{
    mu_assert_msg(run_table_and_psnr_tests());
    mu_assert_msg(run_ssim_tests());
    mu_assert_msg(run_motion_tests());
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
