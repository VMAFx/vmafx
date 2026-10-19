/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * float_motion CPU vs. Metal: the twin must return the CPU's scores bit for
 * bit and accept the CPU's options. First added as a places=4 test on one
 * two-frame fixture (PR #1018); the CUDA, SYCL and HIP twins are exact
 * (ADR-1409, ADR-1411, test_cuda_float_motion_parity.c), and every case below
 * compares each output of every frame with == . Three state rows meet here:
 *
 *  - T-GPU-FLOAT-MOTION-CPU-FLOAT-SUM-2026-10-01: the CPU adds each row's
 *    |cur - prev| left to right into one fp32 accumulator and the rows into
 *    another; a per-block sum is 1.36e-4 off on 1080p checkerboards.
 *    test_float_motion_*_exact compare motion, motion2 and motion3 on every
 *    frame with the default options, on 8, 10 and 12 bit noise (row sums pass
 *    2^11 and round at every step), on a 960x540 noise frame and on a 1920x1080
 *    checkerboard; the add_scale1 / add_uv cases add the half-size term and the
 *    chroma planes.
 *  - T-GPU-FLOAT-MOTION3-MISSING-2026-09-30, Metal part:
 *    test_float_motion_motion3 (VMAF_feature_motion3_score present and == with
 *    the default options and with motion_blend_factor / motion_blend_offset) and
 *    test_float_motion_one_frame (a one-frame input emits the CPU's values).
 *  - T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26, float_motion part:
 *    test_float_motion_max_val (motion_max_val accepted and ==, a zero cap
 *    included), test_float_motion_fps_weight_debug_score (the debug
 *    VMAF_feature_motion_score goes through the CPU's motion_clip(), so it
 *    carries motion_fps_weight as the CPU's does), test_float_motion_force_zero
 *    and test_float_motion_max_val_out_of_range (both sides refuse 10001).
 *
 * Fixtures are synthetic and deterministic; the chroma planes carry noise too,
 * so motion_add_uv has terms to add. The macOS tester bundle runs this test
 * and reports each case (ADR-1496).
 *
 * Skip behaviour: a case reports "[skip: no Metal device]" and the run exits
 * 77 when no Metal device is visible.
 */

#include "float_bits.h"
#include "metal_twin.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "feature/feature_extractor.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#define CPU_NAME "float_motion"
#define TWIN_NAME METAL_TWIN("float_motion_metal", CPU_NAME)

#define MAX_FRAMES 8u
#define MAX_KEYS 3u
#define MAX_OPTS 3u
#define NAME_LEN 96u
/* A key is "<base>_<suffix>": the longest base ("motion2" / "motion3", 7 bytes), an underscore and
 * a suffix of up to NAME_LEN - 1 bytes, so it cannot be truncated. */
#define KEY_LEN (NAME_LEN + 16u)

typedef enum FixtureKind {
    FIXTURE_TEXTURE, /* texture that moves a few samples a frame */
    FIXTURE_NOISE,   /* uncorrelated samples, new ones every frame */
    FIXTURE_CHECKER, /* checkerboard whose phase flips every frame */
} FixtureKind;

typedef struct Fixture {
    FixtureKind kind;
    unsigned bpc;
    unsigned w;
    unsigned h;
} Fixture;

/* One option set, a frame count and the outputs the CPU has under it. */
typedef struct Scenario {
    const char *opts[(2u * MAX_OPTS) + 1u]; /* key, value, ... ; NULL-terminated */
    unsigned frames;
    bool nonzero;                    /* the first key must not be 0 on every frame */
    const char *keys[MAX_KEYS + 1u]; /* NULL-terminated; the debug motion first */
} Scenario;

typedef double Scores[MAX_FRAMES][MAX_KEYS];

static const Fixture FX_TEXTURE8 = {FIXTURE_TEXTURE, 8u, 256u, 144u};
static const Fixture FX_NOISE8 = {FIXTURE_NOISE, 8u, 256u, 144u};
static const Fixture FX_NOISE10 = {FIXTURE_NOISE, 10u, 256u, 144u};
static const Fixture FX_NOISE12 = {FIXTURE_NOISE, 12u, 256u, 144u};
static const Fixture FX_NOISE_960 = {FIXTURE_NOISE, 8u, 960u, 540u};
static const Fixture FX_CHECKER_1080 = {FIXTURE_CHECKER, 8u, 1920u, 1080u};
/* Odd 4:2:0 geometry, as the CUDA option cases use. */
static const Fixture FX_OPTIONS = {FIXTURE_TEXTURE, 8u, 161u, 91u};

#define DEFAULT_KEYS                                                                               \
    {"VMAF_feature_motion_score", "VMAF_feature_motion2_score", "VMAF_feature_motion3_score"}

static const Scenario SC_DEFAULT3 = {{NULL}, 3u, true, DEFAULT_KEYS};
static const Scenario SC_DEFAULT6 = {{NULL}, 6u, true, DEFAULT_KEYS};
/* One frame: the CPU publishes motion2 = motion = 0 from extract() and
 * motion3 = 0 from flush(). */
static const Scenario SC_ONE_FRAME = {{NULL}, 1u, false, DEFAULT_KEYS};
static const Scenario SC_SCALE1 = {
    {"motion_add_scale1", "true", NULL}, 3u, true, {"motion_mdc", "motion2_mdc", "motion3_mdc"}};
static const Scenario SC_UV = {
    {"motion_add_uv", "true", NULL}, 3u, true, {"motion_mau", "motion2_mau", "motion3_mau"}};
static const Scenario SC_SCALE1_UV = {{"motion_add_scale1", "true", "motion_add_uv", "true", NULL},
                                      3u,
                                      true,
                                      {"motion_mdc_mau", "motion2_mdc_mau", "motion3_mdc_mau"}};
static const Scenario SC_FORCE_ZERO = {{"motion_force_zero", "true", NULL},
                                       6u,
                                       false,
                                       {"motion_force_0", "motion2_force_0", "motion3_force_0"}};

/* ------------------------------------------------------------------ */
/* Fixtures                                                           */
/* ------------------------------------------------------------------ */

/* lowbias32 hash of the position, frame and plane: stateless, so both runs
 * see the same pictures. */
static uint32_t sample_hash(unsigned row, unsigned col, unsigned frame, unsigned plane)
{
    uint32_t x =
        ((uint32_t)row << 16) ^ (uint32_t)col ^ (frame * 0x9E3779B9u) ^ (plane * 0x85EBCA6Bu);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* 8-bit code of the moving texture or the checkerboard at a luma position. */
static unsigned structured_code(FixtureKind kind, unsigned row, unsigned col, unsigned frame,
                                uint32_t hash)
{
    if (kind == FIXTURE_CHECKER) {
        const unsigned parity = ((row / 5u) + (col / 5u) + frame) & 1u;
        return (parity ? 200u : 40u) + (hash & 7u);
    }
    const unsigned r = row + (frame * 3u);
    const unsigned c = col + frame;
    const unsigned v = (((r * 3u) + (c * 2u)) & 0xFFu) ^ (((r >> 2) * (c >> 3)) & 0x1Fu);
    return (v + (frame * frame)) & 0xFFu;
}

/* Sample of the fixture. Above 8 bits the low bits vary too, so the 1/scaler
 * conversion is exercised. Chroma is noise on every fixture. */
static unsigned sample_code(const Fixture *fx, unsigned plane, unsigned row, unsigned col,
                            unsigned frame)
{
    const uint32_t hash = sample_hash(row, col, frame, plane);
    if (plane > 0u || fx->kind == FIXTURE_NOISE) {
        return hash >> (32u - fx->bpc);
    }
    const unsigned low_bits = fx->bpc - 8u;
    const unsigned code = structured_code(fx->kind, row, col, frame, hash);
    return (code << low_bits) | ((hash >> 24) & ((1u << low_bits) - 1u));
}

static void put_sample(VmafPicture *pic, unsigned plane, unsigned row, unsigned col, unsigned v)
{
    uint8_t *line = (uint8_t *)pic->data[plane] + ((size_t)row * (size_t)pic->stride[plane]);
    if (pic->bpc <= 8u) {
        line[col] = (uint8_t)v;
    } else {
        ((uint16_t *)line)[col] = (uint16_t)v;
    }
}

static int fill_picture(VmafPicture *pic, const Fixture *fx, unsigned frame)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, fx->bpc, fx->w, fx->h);
    if (err) {
        return err;
    }
    for (unsigned p = 0; p < 3u; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                put_sample(pic, p, row, col, sample_code(fx, p, row, col, frame));
            }
        }
    }
    return 0;
}

/* Frame `frame` through `vmaf`, which takes both pictures. Motion reads the
 * reference only; the distorted picture is the same frame. */
static int feed_frame(VmafContext *vmaf, const Fixture *fx, unsigned frame)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_picture(&ref, fx, frame);
    if (err) {
        return err;
    }
    err = fill_picture(&dist, fx, frame);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, frame);
}

/* ------------------------------------------------------------------ */
/* One extractor, on the CPU or on the twin                           */
/* ------------------------------------------------------------------ */

typedef struct Side {
    VmafContext *vmaf;
    void *state;
} Side;

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
    /* vmaf_use_feature() takes the dictionary over, on failure too. */
    return vmaf_use_feature(vmaf, name, dict);
}

static int side_open(Side *side, const char *name, const char *const *opts)
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    int err = vmaf_init(&side->vmaf, cfg);
    if (!err && side->state) {
        err = metal_twin_import(side->vmaf, side->state);
    }
    return err ? err : use_feature(side->vmaf, name, opts);
}

/* The Metal state is freed only after its context is closed (ADR-0157). */
static int side_close(Side *side)
{
    int err = side->vmaf ? vmaf_close(side->vmaf) : 0;
    side->vmaf = NULL;
    if (side->state) {
        const int freed = metal_twin_close(side->state);
        err = err ? err : freed;
        side->state = NULL;
    }
    return err;
}

static char *feed_all(VmafContext *vmaf, const Fixture *fx, unsigned frames)
{
    for (unsigned i = 0; i < frames; i++) {
        mu_assert("feeding a frame failed", !feed_frame(vmaf, fx, i));
    }
    mu_assert("vmaf_read_pictures(EOS) failed", !vmaf_read_pictures(vmaf, NULL, NULL, 0));
    return NULL;
}

static size_t key_count(const Scenario *sc)
{
    size_t n = 0u;
    while (n < MAX_KEYS && sc->keys[n] != NULL) {
        n++;
    }
    return n;
}

static char *read_scores(VmafContext *vmaf, const char *name, const Scenario *sc, Scores out)
{
    for (unsigned i = 0; i < sc->frames; i++) {
        for (unsigned k = 0; k < key_count(sc); k++) {
            if (vmaf_feature_score_at_index(vmaf, sc->keys[k], &out[i][k], i)) {
                (void)fprintf(stderr, "\n%s: no score for %s at frame %u\n", name, sc->keys[k], i);
                return "an output the CPU extractor has is missing";
            }
        }
    }
    return NULL;
}

/* The scenario's frames through extractor `name`, on the twin when
 * `on_twin`. Sets `*skipped` and mu_skipped, and returns NULL, when there is
 * no Metal device. */
static char *run_side(const char *name, bool on_twin, const Scenario *sc, const Fixture *fx,
                      Scores out, bool *skipped)
{
    Side side = {NULL, NULL};
    if (on_twin && (metal_twin_open(&side.state) != 0 || side.state == NULL)) {
        (void)fprintf(stderr, "[skip: no Metal device] ");
        mu_skipped = 1;
        *skipped = true;
        return NULL;
    }
    char *msg = NULL;
    if (side_open(&side, name, sc->opts)) {
        msg = on_twin ? "the Metal twin rejected the CPU's options" :
                        "the CPU extractor rejected the options";
    } else {
        msg = feed_all(side.vmaf, fx, sc->frames);
    }
    if (!msg) {
        msg = read_scores(side.vmaf, name, sc, out);
    }
    const int closed = side_close(&side);
    return (!msg && closed) ? "closing a context failed" : msg;
}

/* ------------------------------------------------------------------ */
/* Comparison                                                         */
/* ------------------------------------------------------------------ */

static unsigned count_mismatches(const Scenario *sc, const Fixture *fx, Scores cpu, Scores twin)
{
    unsigned bad = 0u;
    for (unsigned i = 0; i < sc->frames; i++) {
        for (unsigned k = 0; k < key_count(sc); k++) {
            if (isfinite(cpu[i][k]) && vmaf_test_identical_f64(cpu[i][k], twin[i][k])) {
                continue;
            }
            bad++;
            (void)fprintf(stderr, "\n%ux%u %u-bit frame %u %s: cpu=%.17g metal=%.17g delta=%.3e\n",
                          fx->w, fx->h, fx->bpc, i, sc->keys[k], cpu[i][k], twin[i][k],
                          fabs(cpu[i][k] - twin[i][k]));
        }
    }
    return bad;
}

/* The first key moves on some frame, so the comparison is not of zeros. */
static bool fixture_moves(const Scenario *sc, Scores cpu)
{
    for (unsigned i = 0; i < sc->frames; i++) {
        if (cpu[i][0] != 0.0) {
            return true;
        }
    }
    return false;
}

/* The twin's scores of the scenario on `fx` are the CPU extractor's, bit for
 * bit. */
static char *check_scenario(const Scenario *sc, const Fixture *fx)
{
    Scores cpu = {{0.0}};
    Scores twin = {{0.0}};
    bool skipped = false;
    mu_assert_msg(run_side(TWIN_NAME, true, sc, fx, twin, &skipped));
    if (skipped) {
        return NULL;
    }
    mu_assert_msg(run_side(CPU_NAME, false, sc, fx, cpu, &skipped));
    const unsigned bad = count_mismatches(sc, fx, cpu, twin);
    mu_assert("float_motion_metal does not return the CPU extractor's scores bit for bit",
              bad == 0u);
    mu_assert("the fixture has no motion", !sc->nonzero || fixture_moves(sc, cpu));
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Scenarios built at run time                                        */
/* ------------------------------------------------------------------ */

/* The three float_motion outputs under `opts`, named with the option suffix
 * the CPU gives them (feature_name.cpp: the alias, then _<alias>_<value>). */
static char *option_case(const Fixture *fx, const char *const *opts, const char *suffix,
                         bool nonzero)
{
    static const char *const bases[MAX_KEYS] = {"motion", "motion2", "motion3"};
    char names[MAX_KEYS][KEY_LEN];
    Scenario sc = {{NULL}, 6u, nonzero, {NULL}};
    for (unsigned i = 0; i < MAX_KEYS; i++) {
        (void)snprintf(names[i], KEY_LEN, "%s_%s", bases[i], suffix);
        sc.keys[i] = names[i];
    }
    for (unsigned i = 0; i < (2u * MAX_OPTS) && opts[i] != NULL; i++) {
        sc.opts[i] = opts[i];
    }
    return check_scenario(&sc, fx);
}

/* Midpoint of the CPU's motion2 range on the option fixture, so a cap or a
 * blend offset there clips some frames and leaves others. */
static char *motion_midpoint(double *mid)
{
    static const Scenario sc = {{NULL}, 6u, true, {"VMAF_feature_motion2_score"}};
    Scores cpu = {{0.0}};
    bool skipped = false;
    mu_assert_msg(run_side(CPU_NAME, false, &sc, &FX_OPTIONS, cpu, &skipped));
    double lo = INFINITY;
    double hi = 0.0;
    for (unsigned i = 1; i < sc.frames; i++) {
        lo = cpu[i][0] < lo ? cpu[i][0] : lo;
        hi = cpu[i][0] > hi ? cpu[i][0] : hi;
    }
    *mid = hi > lo ? floor((lo + hi) * 5.0) / 10.0 : -1.0;
    mu_assert("the fixture's motion does not span a clip point", *mid > 0.0);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Cases                                                              */
/* ------------------------------------------------------------------ */

static char *test_float_motion_metal_registered(void)
{
    const VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(TWIN_NAME);
    mu_assert("the float_motion twin must be registered", fex != NULL);
    mu_assert("the float_motion twin's name matches", !strcmp(fex->name, TWIN_NAME));
    return NULL;
}

static char *test_float_motion_ramp_8bit_exact(void)
{
    return check_scenario(&SC_DEFAULT3, &FX_TEXTURE8);
}

static char *test_float_motion_noise_8bit_exact(void)
{
    return check_scenario(&SC_DEFAULT3, &FX_NOISE8);
}

static char *test_float_motion_noise_10bit_exact(void)
{
    return check_scenario(&SC_DEFAULT3, &FX_NOISE10);
}

static char *test_float_motion_noise_12bit_exact(void)
{
    return check_scenario(&SC_DEFAULT3, &FX_NOISE12);
}

/* The 960x540 frame and the 1080p checkerboard are where a per-block sum
 * left the CPU's row-by-row fp32 sum (1.36e-4 on the checkerboard). */
static char *test_float_motion_960x540_noise_exact(void)
{
    return check_scenario(&SC_DEFAULT3, &FX_NOISE_960);
}

static char *test_float_motion_1080p_checkerboard_exact(void)
{
    return check_scenario(&SC_DEFAULT3, &FX_CHECKER_1080);
}

static char *test_float_motion_add_scale1_exact(void)
{
    return check_scenario(&SC_SCALE1, &FX_NOISE8);
}

static char *test_float_motion_add_uv_exact(void)
{
    return check_scenario(&SC_UV, &FX_NOISE8);
}

static char *test_float_motion_add_scale1_and_uv_exact(void)
{
    return check_scenario(&SC_SCALE1_UV, &FX_NOISE8);
}

/* motion_max_val at the midpoint of the fixture's motion2 (the cap clips
 * some frames), and a zero cap, which clips every score to exactly zero. */
static char *test_float_motion_max_val(void)
{
    double cap = 0.0;
    mu_assert_msg(motion_midpoint(&cap));
    char value[NAME_LEN];
    char suffix[NAME_LEN];
    (void)snprintf(value, sizeof(value), "%.17g", cap);
    (void)snprintf(suffix, sizeof(suffix), "mmxv_%g", cap);
    const char *const opts[] = {"motion_max_val", value, NULL};
    mu_assert_msg(option_case(&FX_OPTIONS, opts, suffix, true));
    if (mu_skipped) {
        return NULL;
    }
    const char *const zero_opts[] = {"motion_max_val", "0", NULL};
    return option_case(&FX_OPTIONS, zero_opts, "mmxv_0", false);
}

/* The debug `motion` score is motion_clip()ped like motion2, so it carries
 * motion_fps_weight on the CPU. */
static char *test_float_motion_fps_weight_debug_score(void)
{
    const char *const opts[] = {"motion_fps_weight", "2", NULL};
    return option_case(&FX_OPTIONS, opts, "mfw_2", true);
}

/* motion3 with the default options, and with the blend options: an offset at
 * the midpoint of the fixture's motion so the blend moves some frames and
 * leaves others. */
static char *test_float_motion_motion3(void)
{
    mu_assert_msg(check_scenario(&SC_DEFAULT6, &FX_OPTIONS));
    if (mu_skipped) {
        return NULL;
    }
    double mid = 0.0;
    mu_assert_msg(motion_midpoint(&mid));
    char offset[NAME_LEN];
    char suffix[NAME_LEN];
    (void)snprintf(offset, sizeof(offset), "%.17g", mid);
    (void)snprintf(suffix, sizeof(suffix), "mbf_0.5_mbo_%g", mid);
    const char *const opts[] = {"motion_blend_factor", "0.5", "motion_blend_offset", offset, NULL};
    return option_case(&FX_OPTIONS, opts, suffix, true);
}

static char *test_float_motion_one_frame(void)
{
    return check_scenario(&SC_ONE_FRAME, &FX_OPTIONS);
}

static char *test_float_motion_force_zero(void)
{
    return check_scenario(&SC_FORCE_ZERO, &FX_OPTIONS);
}

/* motion_max_val = 10000 is the declared maximum: the twin must accept it and
 * score like the CPU (the default cap, so the names carry no suffix). Only
 * then does its refusal of 10001 mean something: a twin without the option
 * refuses every value. */
static char *test_float_motion_max_val_out_of_range(void)
{
    static const char *const bad_opts[] = {"motion_max_val", "10001", NULL};
    Scenario in_range = SC_DEFAULT6;
    in_range.opts[0] = "motion_max_val";
    in_range.opts[1] = "10000";
    in_range.opts[2] = NULL;
    mu_assert_msg(check_scenario(&in_range, &FX_OPTIONS));
    if (mu_skipped) {
        return NULL;
    }
    Side cpu = {NULL, NULL};
    Side twin = {NULL, NULL};
    mu_assert("no Metal state", metal_twin_open(&twin.state) == 0 && twin.state != NULL);
    const int cpu_err = side_open(&cpu, CPU_NAME, bad_opts);
    const int twin_err = side_open(&twin, TWIN_NAME, bad_opts);
    (void)side_close(&cpu);
    (void)side_close(&twin);
    mu_assert("the CPU accepted an out-of-range motion_max_val", cpu_err != 0);
    mu_assert("the Metal twin accepted an out-of-range motion_max_val", twin_err != 0);
    return NULL;
}

static void run_exact_cases(void)
{
    metal_run_case(test_float_motion_ramp_8bit_exact);
    metal_run_case(test_float_motion_noise_8bit_exact);
    metal_run_case(test_float_motion_noise_10bit_exact);
    metal_run_case(test_float_motion_noise_12bit_exact);
    metal_run_case(test_float_motion_960x540_noise_exact);
    metal_run_case(test_float_motion_1080p_checkerboard_exact);
}

static void run_chroma_cases(void)
{
    metal_run_case(test_float_motion_add_scale1_exact);
    metal_run_case(test_float_motion_add_uv_exact);
    metal_run_case(test_float_motion_add_scale1_and_uv_exact);
}

static void run_option_cases(void)
{
    metal_run_case(test_float_motion_max_val);
    metal_run_case(test_float_motion_fps_weight_debug_score);
    metal_run_case(test_float_motion_motion3);
    metal_run_case(test_float_motion_one_frame);
    metal_run_case(test_float_motion_force_zero);
    metal_run_case(test_float_motion_max_val_out_of_range);
}

char *run_tests(void)
{
    metal_run_case(test_float_motion_metal_registered);
    run_exact_cases();
    run_chroma_cases();
    run_option_cases();
    return metal_first_failure;
}

/* NOLINTEND(modernize-use-nullptr) */
