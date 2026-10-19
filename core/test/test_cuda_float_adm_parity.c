/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_adm CPU vs. CUDA parity (ADR-0956 round 4; exact since ADR-1420).
 *
 * The float-path ADM extractor is float_adm.c / adm.c / adm_tools.c on the
 * CPU and float_adm_cuda.c plus float_adm/float_adm_score.cu on CUDA. Since
 * ADR-1420 the kernels run the reference's arithmetic operation for
 * operation, the row sums are added in the reference's order and the host
 * concludes with the reference's own routines, so this test asserts equality,
 * not a tolerance: every output of every exact case has the CPU's bits, the
 * per-scale numerators and denominators of `debug=true` included.
 *
 * Before ADR-1420 the twin divided where the reference multiplies by a
 * refined reciprocal estimate, associated the angle test's threshold
 * differently, formed the masking threshold in another order with fp32
 * constants, reduced each row in tiles and floored the frame sums at 1e-2
 * where the reference floors them at 1e-10. Every exact case below fails on
 * that twin; the isolated-sample case fails by a whole unit of adm2.
 *
 * One option is not exact: adm_p_norm other than 3 raises each term with
 * powf(), and the device's powf is not glibc's. That case keeps a tolerance.
 *
 * Skip behaviour: if vmaf_cuda_state_init() fails (no CUDA driver / no device)
 * the test emits "[skip: no CUDA device]" and passes.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

/* Fixture geometry — large enough for the 4-scale ADM DWT pyramid. */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif

#define MAX_KEYS 18u

/* Device powf() against glibc's, through the p-norm root and the score
 * ratio. Measured on the Netflix pair, both checkerboard pairs and 50 4K
 * frames at adm_p_norm 2, 4.5 and 20: 1.1e-7 at most. */
#define P_NORM_TOL 1e-6

typedef enum Content {
    CONTENT_TEXTURE,  /* a textured ramp with a position-dependent error */
    CONTENT_NOISE,    /* independent hashes: every band and angle populated */
    CONTENT_CONTRAST, /* the reference with its contrast raised by a quarter */
    CONTENT_ISOLATED, /* a flat frame; the reference has one sample one level up */
} Content;

/* One frame geometry of a case. */
typedef struct Fixture {
    unsigned w;
    unsigned h;
    unsigned bpc;
    Content content;
} Fixture;

/* Deterministic position hash. */
static unsigned position_hash(unsigned row, unsigned col, unsigned salt)
{
    unsigned x = row * 73856093u ^ col * 19349663u ^ salt * 83492791u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return x;
}

static unsigned textured_luma(unsigned row, unsigned col, bool distorted)
{
    unsigned v = ((row * 3u + col * 2u) & 0xFFu) ^ (((row >> 2) * (col >> 3)) & 0x1Fu);
    if (distorted)
        v += 9u + ((row * 5u + col * 7u) % 11u);
    return v & 0xFFu;
}

static unsigned noise_luma(unsigned row, unsigned col, bool distorted)
{
    const unsigned base = 64u + (position_hash(row >> 1, col >> 1, 3u) & 0x7Fu);
    if (!distorted)
        return base;
    return base + (position_hash(row, col, 4u) % 17u);
}

/* A reference with detail and the same picture at 1.25 times the contrast:
 * every band of the distorted picture is the reference's times 1.25, so the
 * decouple's angle test passes everywhere and the restored signal is bounded
 * by the enhancement gain limit. */
static unsigned contrast_luma(unsigned row, unsigned col, bool distorted)
{
    const int detail = (int)(position_hash(row, col, 5u) % 81u) - 40;
    return (unsigned)(128 + (distorted ? (detail * 5) / 4 : detail));
}

/* Luma in units of the bit depth's least significant level for the isolated
 * fixture, in units of an 8-bit level otherwise. */
static unsigned fixture_luma(const Fixture *fx, unsigned row, unsigned col, bool distorted)
{
    const unsigned gain = 1u << (fx->bpc - 8u);
    switch (fx->content) {
    case CONTENT_NOISE:
        return noise_luma(row, col, distorted) * gain;
    case CONTENT_CONTRAST:
        return contrast_luma(row, col, distorted) * gain;
    case CONTENT_ISOLATED:
        return 128u * gain + ((!distorted && row == fx->h / 2u && col == fx->w / 2u) ? 1u : 0u);
    case CONTENT_TEXTURE:
    default:
        return textured_luma(row, col, distorted) * gain;
    }
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
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++)
            put_sample(pic, 0u, row, col, fixture_luma(fx, row, col, distorted));
    }
    for (unsigned p = 1; p < 3; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++)
                put_sample(pic, p, row, col, 128u << (fx->bpc - 8u));
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

/* One scoring run of the CPU `float_adm` or the CUDA `float_adm_cuda`
 * extractor. */
typedef struct {
    bool use_cuda;
    VmafContext *vmaf;
    VmafCudaState *cu_state;
} AdmRun;

/* Create the context (and the CUDA state for the CUDA twin). `*skipped` is set
 * when the CUDA leg finds no device. */
static char *adm_run_init(AdmRun *run, int *skipped)
{
    if (run->use_cuda) {
        VmafCudaConfiguration cuda_cfg = {0};
        if (vmaf_cuda_state_init(&run->cu_state, cuda_cfg) != 0 || run->cu_state == NULL) {
            (void)fprintf(stderr, "[skip: no CUDA device] ");
            mu_skipped = 1;
            *skipped = 1;
            return NULL;
        }
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    mu_assert("vmaf_init failed", !vmaf_init(&run->vmaf, cfg));
    if (run->use_cuda) {
        mu_assert("vmaf_cuda_import_state failed",
                  !vmaf_cuda_import_state(run->vmaf, run->cu_state));
    }
    return NULL;
}

/* Open the run with `opts` (NULL for defaults), which it always takes over:
 * vmaf_use_feature() consumes it, and it is freed here when the run never gets
 * that far. */
static char *adm_run_open(AdmRun *run, VmafFeatureDictionary *opts, int *skipped)
{
    *skipped = 0;
    char *msg = adm_run_init(run, skipped);
    if (msg || *skipped) {
        (void)vmaf_feature_dictionary_free(&opts);
        return msg;
    }
    mu_assert("vmaf_use_feature failed",
              !vmaf_use_feature(run->vmaf, run->use_cuda ? "float_adm_cuda" : "float_adm", opts));
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
                          run->use_cuda ? "CUDA" : "CPU");
            return "float ADM feature-name key not emitted";
        }
    }
    return NULL;
}

static char *adm_run_close(AdmRun *run)
{
    if (run->vmaf)
        mu_assert("vmaf_close failed", !vmaf_close(run->vmaf));
    if (run->cu_state)
        mu_assert("vmaf_cuda_state_free failed", !vmaf_cuda_state_free(run->cu_state));
    return NULL;
}

/* Score one frame on the CPU or CUDA twin and read `keys` into `out`. Without a
 * CUDA device the CUDA leg is skipped and `out` stays NaN. */
static char *run_adm(bool use_cuda, const Fixture *fx, VmafFeatureDictionary *opts,
                     const char *const *keys, size_t count, double *out)
{
    for (size_t k = 0; k < count; k++)
        out[k] = NAN;
    AdmRun run = {.use_cuda = use_cuda};
    int skipped = 0;
    char *msg = adm_run_open(&run, opts, &skipped);
    if (!msg && !skipped)
        msg = adm_run_score(&run, fx, keys, count, out);
    char *close_msg = adm_run_close(&run);
    return msg ? msg : close_msg;
}

/* One case: a fixture, an option (NULL for none) and the keys to compare. */
typedef struct Case {
    const char *what;
    Fixture fx;
    const char *option;
    const char *value;
    bool debug;
    const char *const *keys;
    size_t count;
} Case;

static VmafFeatureDictionary *case_opts(const Case *c)
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

/* Every key of the case within `tol` of the CPU on the CUDA twin; tol = 0 is
 * equality. A skipped CUDA leg passes. `cpu_out` (may be NULL) receives the
 * CPU's values. */
static char *check_case(const Case *c, double tol, double *cpu_out)
{
    double cpu[MAX_KEYS];
    double gpu[MAX_KEYS];
    mu_assert("check_case: too many keys", c->count <= MAX_KEYS);
    char *msg = run_adm(false, &c->fx, case_opts(c), c->keys, c->count, cpu);
    if (msg)
        return msg;
    if (cpu_out)
        memcpy(cpu_out, cpu, c->count * sizeof(double));
    msg = run_adm(true, &c->fx, case_opts(c), c->keys, c->count, gpu);
    if (msg || isnan(gpu[0]))
        return msg;
    unsigned mismatches = 0u;
    for (size_t k = 0; k < c->count; k++) {
        mu_assert("CPU float_adm output is non-finite", isfinite(cpu[k]));
        if (cpu[k] == gpu[k] || fabs(cpu[k] - gpu[k]) <= tol)
            continue;
        mismatches++;
        (void)fprintf(stderr, "\n%s %ux%u %u-bit %s: cpu=%.17g cuda=%.17g delta=%.3e\n", c->what,
                      c->fx.w, c->fx.h, c->fx.bpc, c->keys[k], cpu[k], gpu[k],
                      fabs(cpu[k] - gpu[k]));
    }
    mu_assert("float_adm_cuda differs from the CPU extractor", mismatches == 0u);
    return NULL;
}

/* The seven scores under an option's feature-name suffix, and the sums
 * `debug=true` adds. The debug ratio `adm` is filed without the suffix on the
 * CPU and on the twin (ADR-2056), so option cases end with it unsuffixed. */
#define SCORE_KEYS(suffix)                                                                         \
    "adm2" suffix, "aim" suffix, "adm3" suffix, "adm_scale0" suffix, "adm_scale1" suffix,          \
        "adm_scale2" suffix, "adm_scale3" suffix
#define SUM_KEYS(suffix)                                                                           \
    "adm_num" suffix, "adm_den" suffix, "adm_num_scale0" suffix, "adm_den_scale0" suffix,          \
        "adm_num_scale1" suffix, "adm_den_scale1" suffix, "adm_num_scale2" suffix,                 \
        "adm_den_scale2" suffix, "adm_num_scale3" suffix, "adm_den_scale3" suffix

static const char *const DEBUG_KEYS[] = {
    "VMAF_feature_adm2_score",
    "VMAF_feature_aim_score",
    "VMAF_feature_adm3_score",
    "VMAF_feature_adm_scale0_score",
    "VMAF_feature_adm_scale1_score",
    "VMAF_feature_adm_scale2_score",
    "VMAF_feature_adm_scale3_score",
    "adm",
    SUM_KEYS(""),
};
#define NUM_DEBUG_KEYS (sizeof(DEBUG_KEYS) / sizeof(DEBUG_KEYS[0]))

#define OPTION_KEYS(name, suffix)                                                                  \
    static const char *const name[] = {SCORE_KEYS(suffix), SUM_KEYS(suffix), "adm"}
OPTION_KEYS(EGL_KEYS, "_egl_1.2");
OPTION_KEYS(BCM_KEYS, "_bcm_1");
OPTION_KEYS(NW_KEYS, "_nw_0");
OPTION_KEYS(SASC_KEYS, "_sasc_1");
OPTION_KEYS(NVD_KEYS, "_nvd_1.5");
OPTION_KEYS(SCF_KEYS, "_scf_2");
#define NUM_OPTION_KEYS (sizeof(EGL_KEYS) / sizeof(EGL_KEYS[0]))
/* The scores only: the sums are fp32 values in the hundreds, where one unit
 * in the last place is 1.5e-5. */
static const char *const APN_KEYS[] = {SCORE_KEYS("_apn_2")};
#define NUM_APN_KEYS (sizeof(APN_KEYS) / sizeof(APN_KEYS[0]))

static char *check_default(const char *what, unsigned w, unsigned h, unsigned bpc, Content content)
{
    const Case c = {.what = what,
                    .fx = {w, h, bpc, content},
                    .debug = true,
                    .keys = DEBUG_KEYS,
                    .count = NUM_DEBUG_KEYS};
    return check_case(&c, 0.0, NULL);
}

static char *check_option(const char *what, Content content, const char *option, const char *value,
                          const char *const *keys, double tol)
{
    const Case c = {.what = what,
                    .fx = {FIXTURE_W, FIXTURE_H, 8u, content},
                    .option = option,
                    .value = value,
                    .debug = true,
                    .keys = keys,
                    .count = NUM_OPTION_KEYS};
    return check_case(&c, tol, NULL);
}

/* Default options, every output including the per-scale sums. */
static char *test_float_adm_default_exact(void)
{
    return check_default("float_adm", FIXTURE_W, FIXTURE_H, 8u, CONTENT_TEXTURE);
}

static char *test_float_adm_noise_exact(void)
{
    return check_default("float_adm noise", FIXTURE_W, FIXTURE_H, 8u, CONTENT_NOISE);
}

static char *test_float_adm_10bit_exact(void)
{
    return check_default("float_adm", FIXTURE_W, FIXTURE_H, 10u, CONTENT_NOISE);
}

static char *test_float_adm_12bit_exact(void)
{
    return check_default("float_adm", FIXTURE_W, FIXTURE_H, 12u, CONTENT_TEXTURE);
}

static char *test_float_adm_16bit_exact(void)
{
    return check_default("float_adm", FIXTURE_W, FIXTURE_H, 16u, CONTENT_NOISE);
}

/* Odd at every scale: 322x182 halves to 161x91, 81x46, 41x23 and 21x12. */
static char *test_float_adm_odd_frame_exact(void)
{
    return check_default("float_adm odd", 322u, 182u, 8u, CONTENT_NOISE);
}

/* 17x17 is the smallest frame whose coarsest bands still have two samples a
 * side (9, 5, 3, 2). The reduced region of every scale reaches the band's
 * edges there, so the mirrored and clamped taps of the masking threshold are
 * part of every sum. */
static char *test_float_adm_smallest_frame_exact(void)
{
    return check_default("float_adm smallest", 17u, 17u, 8u, CONTENT_NOISE);
}

static char *test_float_adm_narrow_frame_exact(void)
{
    return check_default("float_adm narrow", 18u, 131u, 8u, CONTENT_NOISE);
}

/* A scale-0 row of the reduced region is 768 samples, longer than any tile of
 * the old reduction, and there are 432 of them. */
static char *test_float_adm_1080p_exact(void)
{
    return check_default("float_adm 1080p", 1920u, 1080u, 8u, CONTENT_NOISE);
}

/* The enhancement gain limit is a double the reference multiplies in fp64:
 * 1.2 is not an fp32 value, and with the contrast fixture the limited product
 * is the restored signal of most samples. */
static char *test_float_adm_gain_limit_exact(void)
{
    return check_option("float_adm egl=1.2", CONTENT_CONTRAST, "adm_enhn_gain_limit", "1.2",
                        EGL_KEYS, 0.0);
}

static char *test_float_adm_bypass_cm_exact(void)
{
    return check_option("float_adm bcm=1", CONTENT_NOISE, "adm_bypass_cm", "1", BCM_KEYS, 0.0);
}

static char *test_float_adm_skip_aim_scale_exact(void)
{
    return check_option("float_adm sasc=1", CONTENT_NOISE, "adm_skip_aim_scale", "1", SASC_KEYS,
                        0.0);
}

static char *test_float_adm_view_dist_exact(void)
{
    return check_option("float_adm nvd=1.5", CONTENT_NOISE, "adm_norm_view_dist", "1.5", NVD_KEYS,
                        0.0);
}

/* ADR-1214 — adm_csf_scale is a no-op in the Watson-97 mode this twin
 * implements, exactly as it is on the CPU (adm_csf_rfactor_s() consults it
 * only in Barten mode), and it files the scores under the CPU's `scf` alias. */
static char *test_float_adm_csf_scale_is_a_watson_mode_noop(void)
{
    return check_option("float_adm scf=2", CONTENT_NOISE, "adm_csf_scale", "2.0", SCF_KEYS, 0.0);
}

/* ADR-1220 — adm_p_norm reaches the kernels. Not exact: both sides raise the
 * terms with powf(), and the device's is not glibc's. */
static char *test_float_adm_p_norm_reaches_kernel(void)
{
    const Case c = {.what = "float_adm apn=2",
                    .fx = {FIXTURE_W, FIXTURE_H, 8u, CONTENT_NOISE},
                    .option = "adm_p_norm",
                    .value = "2.0",
                    .keys = APN_KEYS,
                    .count = NUM_APN_KEYS};
    double cpu[MAX_KEYS];
    double def[MAX_KEYS];
    char *msg = check_case(&c, P_NORM_TOL, cpu);
    if (msg)
        return msg;
    /* The exponent is honoured, not merely accepted: the scores differ from
     * the default exponent's by far more than the tolerance. */
    const Case d = {.what = "float_adm", .fx = c.fx, .keys = DEBUG_KEYS, .count = NUM_APN_KEYS};
    msg = check_case(&d, 0.0, def);
    if (msg)
        return msg;
    mu_assert("adm_p_norm = 2 must change adm2", fabs(cpu[0] - def[0]) > 1e-4);
    return NULL;
}

/* A flat 16-bit frame whose reference has one sample one level up, scored
 * without the noise floor. The reference band energy is then about 3e-4 and
 * the distorted picture has none: the CPU reports adm2 = 0. The twin floored
 * its frame sums at 1e-2 * area / 1080p where the CPU floors them at 1e-10 of
 * that, zeroed the denominator and reported adm2 = 1. */
static char *test_float_adm_small_sums_are_not_floored(void)
{
    const Case c = {.what = "float_adm isolated",
                    .fx = {576u, 324u, 16u, CONTENT_ISOLATED},
                    .option = "adm_noise_weight",
                    .value = "0",
                    .debug = true,
                    .keys = NW_KEYS,
                    .count = NUM_OPTION_KEYS};
    double cpu[MAX_KEYS];
    char *msg = check_case(&c, 0.0, cpu);
    if (msg)
        return msg;
    /* NW_KEYS: [0] adm2, [8] adm_den. */
    mu_assert("the isolated fixture must leave a denominator below the old floor",
              cpu[8] > 0.0 && cpu[8] < 1e-2 * (576.0 * 324.0) / (1920.0 * 1080.0));
    mu_assert("the CPU reports adm2 = 0 for the isolated fixture", cpu[0] == 0.0);
    return NULL;
}

static char *run_exact_bit_depth_cases(void)
{
    mu_run_test(test_float_adm_default_exact);
    mu_run_test(test_float_adm_noise_exact);
    mu_run_test(test_float_adm_10bit_exact);
    mu_run_test(test_float_adm_12bit_exact);
    mu_run_test(test_float_adm_16bit_exact);
    return NULL;
}

static char *run_exact_geometry_cases(void)
{
    mu_run_test(test_float_adm_odd_frame_exact);
    mu_run_test(test_float_adm_smallest_frame_exact);
    mu_run_test(test_float_adm_narrow_frame_exact);
    mu_run_test(test_float_adm_1080p_exact);
    return NULL;
}

static char *run_exact_option_cases(void)
{
    mu_run_test(test_float_adm_gain_limit_exact);
    mu_run_test(test_float_adm_bypass_cm_exact);
    mu_run_test(test_float_adm_skip_aim_scale_exact);
    mu_run_test(test_float_adm_view_dist_exact);
    mu_run_test(test_float_adm_csf_scale_is_a_watson_mode_noop);
    return NULL;
}

/* init() result of the twin for a w x h frame, without a device: the size
 * check comes before any device resource is claimed. */
static int twin_init_result(unsigned w, unsigned h)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_adm_cuda");
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
 * such frames, and so does the twin (it used to clamp the two indices the CPU
 * read out of the band, and so scored frames the CPU cannot). */
static char *test_float_adm_cuda_rejects_frames_below_17(void)
{
    mu_assert("float_adm_cuda must reject 8x8", twin_init_result(8u, 8u) == -EINVAL);
    mu_assert("float_adm_cuda must reject 16x16", twin_init_result(16u, 16u) == -EINVAL);
    mu_assert("float_adm_cuda must reject 17x16", twin_init_result(17u, 16u) == -EINVAL);
    mu_assert("float_adm_cuda must reject 16x17", twin_init_result(16u, 17u) == -EINVAL);
    return NULL;
}

static char *run_other_cases(void)
{
    mu_run_test(test_float_adm_p_norm_reaches_kernel);
    mu_run_test(test_float_adm_small_sums_are_not_floored);
    mu_run_test(test_float_adm_cuda_rejects_frames_below_17);
    return NULL;
}

char *run_tests(void)
{
    mu_assert_msg(run_exact_bit_depth_cases());
    mu_assert_msg(run_exact_geometry_cases());
    mu_assert_msg(run_exact_option_cases());
    mu_assert_msg(run_other_cases());
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
