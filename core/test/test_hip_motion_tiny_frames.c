/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * motion_hip and motion_v2_hip against the scalar CPU `motion` and
 * `motion_v2`, bit for bit, on tiny, odd and 16-bit frames
 * (T-HIP-MOTION-BLUR-THEN-DIFF-2026-09-29, ADR-1377).
 *
 * Since the upstream port of the pipelined motion (Netflix a4a1492d, fork PR
 * #532) the CPU `motion` blurs the frame difference: SAD = sum |blur(prev -
 * cur)|, rounded after the vertical and after the horizontal pass. motion_hip
 * still blurred each frame and differenced the blurred frames, which equals
 * that sum only without rounding; the SYCL twin measured the leftover at
 * 2.0e-4 on 17x17 and 1.3e-5 on the Netflix 576x324 pair before ADR-1371.
 * Both HIP twins now run one shared kernel (integer_motion_sad_hip.c over
 * integer_motion_v2/motion_v2_score.hip), and integer arithmetic leaves no
 * reason for any difference, so this test compares with ==, the debug
 * `motion` score included (it carries motion_clip() like the CPU's).
 *
 * The geometries cover the 3x3 minimum, odd sizes, sizes around the 16x16
 * block and the 20x20 tile it loads (the tile reflects past the plane on
 * 17x17), and a frame above 1280x720. Full-range noise, independent per
 * frame, makes every pixel move; at 16 bits its differences overflow an
 * int32 vertical filter sum, which exercises the kernel's int64 path. Frames
 * below 3x3 must fail init() with -EINVAL, which needs no device.
 *
 * The parity cases skip (exit 77) when no HIP device is visible.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mu_table.h"
#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

#define NUM_FRAMES 4u
#define MAX_KEYS 3u

typedef struct {
    unsigned w;
    unsigned h;
} Geometry;

static const Geometry ACCEPTED[] = {
    {3u, 3u},  {4u, 5u},   {15u, 15u}, {16u, 16u},   {17u, 17u},   {19u, 23u},
    {31u, 9u}, {33u, 33u}, {64u, 64u}, {257u, 145u}, {576u, 324u}, {1283u, 723u},
};
#define NUM_ACCEPTED (sizeof(ACCEPTED) / sizeof(ACCEPTED[0]))

static const Geometry REJECTED[] = {{2u, 3u}, {3u, 2u}, {2u, 2u}};
#define NUM_REJECTED (sizeof(REJECTED) / sizeof(REJECTED[0]))

static const unsigned BIT_DEPTHS[] = {8u, 10u, 16u};
#define NUM_BIT_DEPTHS (sizeof(BIT_DEPTHS) / sizeof(BIT_DEPTHS[0]))

/* A CPU extractor, its HIP twin, the option both get (NULL for none), and
 * the scores both emit. `debug` asks the CPU `motion` for the debug score
 * motion_hip emits by default. */
typedef struct {
    const char *cpu;
    const char *hip;
    const char *opt_key;
    const char *opt_val;
    unsigned n_keys;
    const char *keys[MAX_KEYS];
} Twin;

static const Twin TWINS[] = {
    {"motion",
     "motion_hip",
     "debug",
     "true",
     3u,
     {"VMAF_integer_feature_motion2_score", "VMAF_integer_feature_motion3_score",
      "VMAF_integer_feature_motion_score"}},
    {"motion_v2",
     "motion_v2_hip",
     NULL,
     NULL,
     3u,
     {"VMAF_integer_feature_motion_v2_sad_score", "VMAF_integer_feature_motion2_v2_score",
      "VMAF_integer_feature_motion3_v2_score"}},
};
#define NUM_TWINS (sizeof(TWINS) / sizeof(TWINS[0]))

/* lowbias32 hash of the position and frame: stateless, so both runs see the
 * same pictures. */
static uint32_t sample_hash(unsigned row, unsigned col, unsigned frame)
{
    uint32_t x = ((uint32_t)row << 16) ^ (uint32_t)col ^ (frame * 0x9E3779B9u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* Plane p of `pic`: luma is full-range noise, chroma sits at mid-range
 * (motion reads luma only). */
static void fill_plane(VmafPicture *pic, unsigned p, unsigned bpc, unsigned frame)
{
    for (unsigned row = 0; row < pic->h[p]; row++) {
        for (unsigned col = 0; col < pic->w[p]; col++) {
            const uint32_t v =
                (p == 0u) ? (sample_hash(row, col, frame) >> (32u - bpc)) : (1u << (bpc - 1u));
            if (bpc == 8u) {
                ((uint8_t *)pic->data[p])[(row * pic->stride[p]) + col] = (uint8_t)v;
            } else {
                ((uint16_t *)pic->data[p])[(row * (pic->stride[p] / 2u)) + col] = (uint16_t)v;
            }
        }
    }
}

static int fill_picture(VmafPicture *pic, Geometry g, unsigned bpc, unsigned frame)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, bpc, g.w, g.h);
    if (err) {
        return err;
    }
    for (unsigned p = 0; p < 3u; p++) {
        fill_plane(pic, p, bpc, frame);
    }
    return 0;
}

/* Register `feature` with the twin's option, if it has one. */
static int use_twin_feature(VmafContext *vmaf, const char *feature, const Twin *t)
{
    VmafFeatureDictionary *opts = NULL;
    if (t->opt_key) {
        const int err = vmaf_feature_dictionary_set(&opts, t->opt_key, t->opt_val);
        if (err) {
            return err;
        }
    }
    return vmaf_use_feature(vmaf, feature, opts);
}

/* Feed NUM_FRAMES frames, flush, and read every score of every frame. */
static char *score_sequence(VmafContext *vmaf, const char *feature, const Twin *t, Geometry g,
                            unsigned bpc, double out[NUM_FRAMES][MAX_KEYS])
{
    mu_assert("vmaf_use_feature failed", !use_twin_feature(vmaf, feature, t));
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        mu_assert("reference allocation failed", !fill_picture(&ref, g, bpc, i));
        if (fill_picture(&dist, g, bpc, i)) {
            (void)vmaf_picture_unref(&ref);
            return "distorted allocation failed";
        }
        mu_assert("vmaf_read_pictures failed", !vmaf_read_pictures(vmaf, &ref, &dist, i));
    }
    mu_assert("vmaf_read_pictures(EOS) failed", !vmaf_read_pictures(vmaf, NULL, NULL, 0));
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        for (unsigned k = 0; k < t->n_keys; k++) {
            mu_assert("motion score missing",
                      !vmaf_feature_score_at_index(vmaf, t->keys[k], &out[i][k], i));
        }
    }
    return NULL;
}

static char *score_cpu_scalar(const Twin *t, Geometry g, unsigned bpc,
                              double out[NUM_FRAMES][MAX_KEYS])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .cpumask = ~(uint64_t)0};
    VmafContext *vmaf = NULL;
    mu_assert("CPU: vmaf_init failed", !vmaf_init(&vmaf, cfg));
    char *msg = score_sequence(vmaf, t->cpu, t, g, bpc, out);
    (void)vmaf_close(vmaf);
    return msg;
}

/* A fresh HIP state per case. `*skipped` is set when there is no device. */
static char *score_hip(const Twin *t, Geometry g, unsigned bpc, double out[NUM_FRAMES][MAX_KEYS],
                       int *skipped)
{
    VmafHipState *state = NULL;
    VmafHipConfiguration hip_cfg = {.device_index = -1};
    if (vmaf_hip_state_init(&state, hip_cfg) != 0 || state == NULL) {
        *skipped = 1;
        return NULL;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    char *msg = NULL;
    if (vmaf_init(&vmaf, cfg)) {
        msg = "HIP: vmaf_init failed";
    } else if (vmaf_hip_import_state(vmaf, state)) {
        msg = "HIP: vmaf_hip_import_state failed";
    } else {
        msg = score_sequence(vmaf, t->hip, t, g, bpc, out);
    }
    if (vmaf) {
        (void)vmaf_close(vmaf);
    }
    vmaf_hip_state_free(&state);
    return msg;
}

static char *check_case(const Twin *t, Geometry g, unsigned bpc, int *skipped)
{
    double cpu[NUM_FRAMES][MAX_KEYS];
    double gpu[NUM_FRAMES][MAX_KEYS];
    mu_assert_msg(score_cpu_scalar(t, g, bpc, cpu));
    mu_assert_msg(score_hip(t, g, bpc, gpu, skipped));
    if (*skipped) {
        return NULL;
    }
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        for (unsigned k = 0; k < t->n_keys; k++) {
            if (cpu[i][k] != gpu[i][k]) {
                (void)fprintf(stderr, "\n  %s %ux%u %u-bit frame %u %s: cpu=%.17g hip=%.17g\n",
                              t->hip, g.w, g.h, bpc, i, t->keys[k], cpu[i][k], gpu[i][k]);
                return "HIP motion differs from the scalar CPU";
            }
        }
    }
    return NULL;
}

static char *check_twin(const Twin *t)
{
    for (size_t b = 0; b < NUM_BIT_DEPTHS; b++) {
        for (size_t i = 0; i < NUM_ACCEPTED; i++) {
            int skipped = 0;
            mu_assert_msg(check_case(t, ACCEPTED[i], BIT_DEPTHS[b], &skipped));
            if (skipped) {
                (void)fprintf(stderr, "[skip: no HIP device] ");
                mu_skipped = 1;
                return NULL;
            }
        }
    }
    return NULL;
}

static char *test_motion_hip_matches_scalar_cpu(void)
{
    return check_twin(&TWINS[0]);
}

static char *test_motion_v2_hip_matches_scalar_cpu(void)
{
    return check_twin(&TWINS[1]);
}

/* init() with a zeroed private state. Rejected sizes return before init()
 * touches the device state or the options, so nothing needs closing. */
static int init_rejects(VmafFeatureExtractor *fex, Geometry g)
{
    void *priv = calloc(1, fex->priv_size);
    if (!priv) {
        return 0;
    }
    fex->priv = priv;
    const int rc = fex->init(fex, VMAF_PIX_FMT_YUV420P, 8u, g.w, g.h);
    free(priv);
    fex->priv = NULL;
    return rc == -EINVAL;
}

static char *test_motion_hip_rejects_below_3x3(void)
{
    for (size_t t = 0; t < NUM_TWINS; t++) {
        VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(TWINS[t].hip);
        mu_assert("HIP motion extractor is not registered", fex != NULL);
        for (size_t i = 0; i < NUM_REJECTED; i++) {
            if (!init_rejects(fex, REJECTED[i])) {
                (void)fprintf(stderr, "\n  %s accepted %ux%u\n", TWINS[t].hip, REJECTED[i].w,
                              REJECTED[i].h);
                return "HIP motion must reject frames below 3x3 with -EINVAL";
            }
        }
    }
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_motion_hip_rejects_below_3x3),
        MU_TEST(test_motion_hip_matches_scalar_cpu),
        MU_TEST(test_motion_v2_hip_matches_scalar_cpu),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
