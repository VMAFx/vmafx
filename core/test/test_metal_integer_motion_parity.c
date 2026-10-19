/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * integer_motion CPU vs. Metal: the twin must return every output of the CPU
 * `motion` extractor bit for bit and accept the CPU's options. First added as
 * a places=4 test on one two-frame fixture (ADR-0421); the CUDA, SYCL and HIP
 * twins are exact (ADR-1372, ADR-1373), and every case below compares each
 * output of every frame with == . Two state rows meet here:
 *
 *  - T-METAL-MOTION-BLUR-THEN-DIFF-2026-09-29: the CPU blurs the frame
 *    difference (SAD = sum |blur(prev - cur)|, rounded after the vertical and
 *    after the horizontal pass); the Metal twin blurred each frame and
 *    differenced the blurred frames, which is the same sum only without
 *    rounding. integer_motion2 and every other output must be ==, tiny frames
 *    included: test_integer_motion_tiny_frames_exact (3x3 up to 64x64 at 8, 10
 *    and 16 bit), test_integer_motion_odd_and_large_frames_exact and the
 *    640x480 test_integer_motion_exact_twin_*. The twin also lacked the CPU's
 *    option table, so every option case (motion_fps_weight, motion_max_val,
 *    motion_force_zero, motion_blend_factor / motion_blend_offset, debug) and
 *    the five-frame window (motion_five_frame_twin_parity.h, motion cases)
 *    failed on it.
 *  - T-GPU-MOTION-SAD-SCORE-NOT-EMITTED-2026-10-02: the CPU publishes
 *    VMAF_integer_feature_motion_sad_score at three sites (the first frame,
 *    motion_force_zero and the regular emit). Every scenario below reads it
 *    first, on 11 frames (the first-frame and regular sites; the 11 frames
 *    also cross the batched twins' readback depth), and the force-zero
 *    scenario reads it at the third site.
 *
 * The twin's init() must refuse frames below 3x3 with -EINVAL as the CPU's
 * does (test_integer_motion_metal_rejects_below_3x3). The motion_v2 twin has
 * its own test; here its cases of the shared five-frame header run with the
 * CPU extractor on both sides.
 *
 * Fixtures are synthetic and deterministic. The macOS tester bundle runs this
 * test and reports each case (ADR-1496).
 *
 * Skip behaviour: a case reports "[skip: no Metal device]" and the run exits
 * 77 when no Metal device is visible.
 */

#include "float_bits.h"
#include "metal_twin.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "feature/feature_extractor.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"
#include "motion_five_frame_twin_parity.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#define CPU_NAME "motion"
#define TWIN_NAME METAL_TWIN("integer_motion_metal", CPU_NAME)

#define MAX_FRAMES 11u
#define MAX_KEYS 4u
#define MAX_OPTS 3u
#define NAME_LEN 96u

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

typedef struct Geometry {
    unsigned w;
    unsigned h;
} Geometry;

/* One option set, a frame count and the outputs the CPU has under it. */
typedef struct Scenario {
    const char *opts[(2u * MAX_OPTS) + 1u]; /* key, value, ... ; NULL-terminated */
    unsigned frames;
    bool nonzero;                    /* the first key must not be 0 on every frame */
    const char *keys[MAX_KEYS + 1u]; /* NULL-terminated; the SAD score first */
} Scenario;

typedef double Scores[MAX_FRAMES][MAX_KEYS];

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
    mu_assert("the Metal twin does not return the CPU extractor's scores bit for bit", bad == 0u);
    mu_assert("the fixture has no motion", !sc->nonzero || fixture_moves(sc, cpu));
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Scenarios and fixtures                                             */
/* ------------------------------------------------------------------ */

#define SAD_KEY "VMAF_integer_feature_motion_sad_score"

static const Fixture FX_TEXTURE8 = {FIXTURE_TEXTURE, 8u, 256u, 144u};
static const Fixture FX_TEXTURE10 = {FIXTURE_TEXTURE, 10u, 256u, 144u};
static const Fixture FX_TWIN8 = {FIXTURE_TEXTURE, 8u, 640u, 480u};
static const Fixture FX_TWIN10 = {FIXTURE_TEXTURE, 10u, 640u, 480u};

/* Eleven frames: more than one readback batch of a batched twin plus a tail
 * for flush(). */
static const Scenario SC_DEFAULTS = {
    {NULL},
    11u,
    true,
    {SAD_KEY, "VMAF_integer_feature_motion2_score", "VMAF_integer_feature_motion3_score"}};
static const Scenario SC_DEBUG = {{"debug", "true", NULL},
                                  11u,
                                  true,
                                  {SAD_KEY, "VMAF_integer_feature_motion_score",
                                   "VMAF_integer_feature_motion2_score",
                                   "VMAF_integer_feature_motion3_score"}};
static const Scenario SC_FORCE_ZERO = {
    {"motion_force_zero", "true", NULL},
    11u,
    false,
    {SAD_KEY "_force_0", "integer_motion2_force_0", "integer_motion3_force_0"}};
static const Scenario SC_WEIGHT_CAP = {
    {"debug", "true", "motion_fps_weight", "0.3", "motion_max_val", "0.5", NULL},
    11u,
    true,
    {SAD_KEY "_mfw_0.3_mmxv_0.5", "integer_motion_mfw_0.3_mmxv_0.5",
     "integer_motion2_mfw_0.3_mmxv_0.5", "integer_motion3_mfw_0.3_mmxv_0.5"}};
static const Scenario SC_BLEND = {
    {"motion_blend_factor", "0.5", "motion_blend_offset", "0.2", NULL},
    11u,
    true,
    {SAD_KEY "_mbf_0.5_mbo_0.2", "integer_motion2_mbf_0.5_mbo_0.2",
     "integer_motion3_mbf_0.5_mbo_0.2"}};

/* Four frames, as the exact-twin tests of the other backends run them. */
static const Scenario SC_TWIN_DEFAULT = {
    {NULL},
    4u,
    true,
    {SAD_KEY, "VMAF_integer_feature_motion2_score", "VMAF_integer_feature_motion3_score"}};
static const Scenario SC_TWIN_DEBUG = {{"debug", "true", NULL},
                                       4u,
                                       true,
                                       {SAD_KEY, "VMAF_integer_feature_motion_score",
                                        "VMAF_integer_feature_motion2_score",
                                        "VMAF_integer_feature_motion3_score"}};

/* Ten frames of full-range noise cross a batch once and leave a tail; the
 * debug run with motion_fps_weight = 0.6 pins the weighted debug score. */
static const Scenario SC_TINY = {
    {NULL},
    10u,
    true,
    {SAD_KEY, "VMAF_integer_feature_motion2_score", "VMAF_integer_feature_motion3_score"}};
static const Scenario SC_TINY_DEBUG_WEIGHT = {{"debug", "true", "motion_fps_weight", "0.6", NULL},
                                              10u,
                                              true,
                                              {SAD_KEY "_mfw_0.6", "integer_motion_mfw_0.6",
                                               "integer_motion2_mfw_0.6",
                                               "integer_motion3_mfw_0.6"}};

/* The 3x3 minimum, odd sizes and sizes around the 16x16 block. */
static const Geometry TINY[] = {
    {3u, 3u}, {4u, 5u}, {17u, 17u}, {19u, 23u}, {33u, 33u}, {31u, 9u}, {64u, 64u},
};
static const Geometry ODD[] = {{257u, 145u}};
static const Geometry LARGE[] = {{1283u, 723u}};
static const Geometry REJECTED[] = {{2u, 3u}, {3u, 2u}, {2u, 2u}};

static const unsigned DEPTHS_ALL[] = {8u, 10u, 16u};
static const unsigned DEPTHS_LARGE[] = {8u, 16u};
static const unsigned DEPTH_8[] = {8u};

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

/* Every geometry at every bit depth, on noise: each pixel moves. */
static char *check_grid(const Scenario *sc, const Geometry *geometries, size_t n_geometries,
                        const unsigned *depths, size_t n_depths)
{
    for (size_t b = 0; b < n_depths; b++) {
        for (size_t i = 0; i < n_geometries; i++) {
            const Fixture fx = {FIXTURE_NOISE, depths[b], geometries[i].w, geometries[i].h};
            mu_assert_msg(check_scenario(sc, &fx));
            if (mu_skipped) {
                return NULL;
            }
        }
    }
    return NULL;
}

/* One scenario at 8 and 10 bits on the 256x144 texture. */
static char *check_8_and_10(const Scenario *sc)
{
    mu_assert_msg(check_scenario(sc, &FX_TEXTURE8));
    if (mu_skipped) {
        return NULL;
    }
    return check_scenario(sc, &FX_TEXTURE10);
}

/* Both depths of the 640x480 frame of the exact-twin tests. */
static char *check_twin_frame(const Scenario *sc)
{
    mu_assert_msg(check_scenario(sc, &FX_TWIN8));
    if (mu_skipped) {
        return NULL;
    }
    return check_scenario(sc, &FX_TWIN10);
}

/* ------------------------------------------------------------------ */
/* Five-frame window (motion_five_frame_twin_parity.h)                */
/* ------------------------------------------------------------------ */

/* The header runs the motion and the motion_v2 cases through one backend;
 * the motion_v2 cases take the CPU extractor on both sides here, since
 * test_metal_motion_v2_parity.c measures that twin. */
static const MftBackend mft_backend = {
    .label = "metal",
    .motion = TWIN_NAME,
    .motion_v2 = "motion_v2",
    .open = metal_twin_open,
    .import = metal_twin_import,
    .close = metal_twin_close,
    /* ADR-2090: a Metal twin's SAD is collected in the next read (its
     * double buffer), so frame i is final one read after the frame after
     * it; the CPU stand-in of the self-test is final earlier. */
    .lag_motion = 2u,
    .lag_motion_v2 = 2u,
};

static unsigned window_failed_cases(unsigned bpc)
{
    bool skipped = false;
    const unsigned failed = mft_failed_cases(&mft_backend, bpc, &skipped);
    if (skipped) {
        mu_skipped = 1;
    }
    return failed;
}

/* ------------------------------------------------------------------ */
/* Cases                                                              */
/* ------------------------------------------------------------------ */

static char *test_integer_motion_metal_registered(void)
{
    const VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(TWIN_NAME);
    mu_assert("the integer_motion twin must be registered", fex != NULL);
    mu_assert("the integer_motion twin's name matches", !strcmp(fex->name, TWIN_NAME));
    return NULL;
}

static char *test_integer_motion_default_exact(void)
{
    return check_8_and_10(&SC_DEFAULTS);
}

static char *test_integer_motion_debug_exact(void)
{
    return check_8_and_10(&SC_DEBUG);
}

/* motion_force_zero: the SAD score's third emit site; zeros on every frame. */
static char *test_integer_motion_force_zero_exact(void)
{
    return check_8_and_10(&SC_FORCE_ZERO);
}

static char *test_integer_motion_weight_and_cap_exact(void)
{
    return check_8_and_10(&SC_WEIGHT_CAP);
}

static char *test_integer_motion_blend_exact(void)
{
    return check_8_and_10(&SC_BLEND);
}

static char *test_integer_motion_exact_twin_default(void)
{
    return check_twin_frame(&SC_TWIN_DEFAULT);
}

static char *test_integer_motion_exact_twin_debug(void)
{
    return check_twin_frame(&SC_TWIN_DEBUG);
}

static char *test_integer_motion_tiny_frames_exact(void)
{
    return check_grid(&SC_TINY, TINY, COUNT_OF(TINY), DEPTHS_ALL, COUNT_OF(DEPTHS_ALL));
}

static char *test_integer_motion_odd_and_large_frames_exact(void)
{
    mu_assert_msg(check_grid(&SC_TINY, ODD, COUNT_OF(ODD), DEPTHS_ALL, COUNT_OF(DEPTHS_ALL)));
    if (mu_skipped) {
        return NULL;
    }
    return check_grid(&SC_TINY, LARGE, COUNT_OF(LARGE), DEPTHS_LARGE, COUNT_OF(DEPTHS_LARGE));
}

static char *test_integer_motion_tiny_frames_debug_weight_exact(void)
{
    return check_grid(&SC_TINY_DEBUG_WEIGHT, TINY, COUNT_OF(TINY), DEPTH_8, COUNT_OF(DEPTH_8));
}

static char *test_integer_motion_five_frame_window_8bit(void)
{
    mu_assert("integer_motion_metal does not return the CPU's five-frame-window scores at 8 bits",
              window_failed_cases(8u) == 0u);
    return NULL;
}

static char *test_integer_motion_five_frame_window_10bit(void)
{
    mu_assert("integer_motion_metal does not return the CPU's five-frame-window scores at 10 bits",
              window_failed_cases(10u) == 0u);
    return NULL;
}

/* init() on a copy of the registered extractor (the registry stays
 * untouched) with a zeroed private state. A rejected size returns before
 * init() reads the Metal state or the options; a twin that wrongly accepts a
 * size has created device state, which close() releases. */
static bool init_rejects(const VmafFeatureExtractor *registered, Geometry g)
{
    VmafFeatureExtractor fex = *registered;
    fex.priv = calloc(1, fex.priv_size);
    if (!fex.priv) {
        return false;
    }
    const int rc = fex.init(&fex, VMAF_PIX_FMT_YUV420P, 8u, g.w, g.h);
    if (rc == 0 && fex.close) {
        (void)fex.close(&fex);
    }
    free(fex.priv);
    return rc == -EINVAL;
}

static char *test_integer_motion_metal_rejects_below_3x3(void)
{
    if (!metal_twin_have_device()) {
        return NULL;
    }
    const VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(TWIN_NAME);
    mu_assert("the integer_motion twin must be registered", fex != NULL);
    for (size_t i = 0; i < COUNT_OF(REJECTED); i++) {
        if (!init_rejects(fex, REJECTED[i])) {
            (void)fprintf(stderr, "\n  %s accepted %ux%u\n", TWIN_NAME, REJECTED[i].w,
                          REJECTED[i].h);
            return "the Metal motion twin must reject frames below 3x3 with -EINVAL";
        }
    }
    return NULL;
}

static void run_option_cases(void)
{
    metal_run_case(test_integer_motion_default_exact);
    metal_run_case(test_integer_motion_debug_exact);
    metal_run_case(test_integer_motion_force_zero_exact);
    metal_run_case(test_integer_motion_weight_and_cap_exact);
    metal_run_case(test_integer_motion_blend_exact);
}

static void run_geometry_cases(void)
{
    metal_run_case(test_integer_motion_exact_twin_default);
    metal_run_case(test_integer_motion_exact_twin_debug);
    metal_run_case(test_integer_motion_tiny_frames_exact);
    metal_run_case(test_integer_motion_odd_and_large_frames_exact);
    metal_run_case(test_integer_motion_tiny_frames_debug_weight_exact);
}

char *run_tests(void)
{
    metal_run_case(test_integer_motion_metal_registered);
    run_option_cases();
    run_geometry_cases();
    metal_run_case(test_integer_motion_five_frame_window_8bit);
    metal_run_case(test_integer_motion_five_frame_window_10bit);
    metal_run_case(test_integer_motion_metal_rejects_below_3x3);
    return metal_first_failure;
}

/* NOLINTEND(modernize-use-nullptr) */
