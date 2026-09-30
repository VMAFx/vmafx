/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * motion_cuda and motion_v2_cuda against the scalar CPU `motion` and
 * `motion_v2`, bit for bit, on tiny, odd and 16-bit frames
 * (T-CUDA-MOTION-BLUR-THEN-DIFF-2026-09-29, ADR-1372).
 *
 * Since the upstream port of the pipelined motion (Netflix a4a1492d, fork PR
 * #532) the CPU `motion` blurs the frame difference: SAD = sum |blur(prev -
 * cur)|, rounded after the vertical and after the horizontal pass. motion_cuda
 * blurred each frame and differenced the blurred frames, which equals that sum
 * only without rounding; the SYCL twin with the same order was 2.0e-4 off at
 * 17x17 and 1.3e-5 on the Netflix 576x324 pair (ADR-1371). Both CUDA twins now
 * run one shared kernel (integer_motion_v2/motion_v2_score.cu through
 * integer_motion_sad_cuda.c), and integer arithmetic leaves no reason for any
 * difference, so this test compares with ==.
 *
 * The geometries cover the 3x3 minimum, odd sizes, sizes around the 16x16
 * block, and one frame above 1280x720. Ten frames cross motion_cuda's
 * eight-frame readback batch once (ADR-0845) and leave a tail for flush().
 * Full-range noise, independent per frame, makes every pixel move; at 16 bits
 * its differences overflow an int32 vertical filter sum, which exercises the
 * kernel's int64 path. A debug run with motion_fps_weight = 0.6 pins the
 * debug `motion` score, which the CPU weights and caps like its SAD score.
 * motion_v2 runs with motion_fps_weight = 0.3 and with motion_max_val = 4 pin
 * the CPU's weighted, capped SAD score and the motion2_v2 / motion3_v2 derived
 * from it, and a one-frame run pins the CPU's 0 / 0 motion2_v2 / motion3_v2.
 * A motion_force_zero run pins the CPU's zeros and the first-frame dispatch:
 * the twin's init() swaps submit() / collect() for extract(), and the engine
 * used to call the cleared submit(). Frames below 3x3 must fail init() with
 * -EINVAL; that case needs no device.
 *
 * The parity cases skip (exit 77) when no CUDA device is visible.
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
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

#define NUM_FRAMES 10u
#define MAX_KEYS 3u

typedef struct {
    unsigned w;
    unsigned h;
} Geometry;

static const Geometry ACCEPTED[] = {
    {3u, 3u},  {4u, 5u},   {17u, 17u},   {19u, 23u},    {33u, 33u},
    {31u, 9u}, {64u, 64u}, {257u, 145u}, {1283u, 723u},
};
#define NUM_ACCEPTED (sizeof(ACCEPTED) / sizeof(ACCEPTED[0]))

static const Geometry REJECTED[] = {{2u, 3u}, {3u, 2u}, {2u, 2u}};
#define NUM_REJECTED (sizeof(REJECTED) / sizeof(REJECTED[0]))

static const unsigned BIT_DEPTHS[] = {8u, 10u, 16u};
#define NUM_BIT_DEPTHS (sizeof(BIT_DEPTHS) / sizeof(BIT_DEPTHS[0]))

/* A CPU extractor, its CUDA twin, the options both take, and the scores both
 * emit. */
typedef struct {
    const char *cpu;
    const char *cuda;
    const char *const *opts;
    unsigned n_keys;
    /* Frames to feed; 0 means NUM_FRAMES. */
    unsigned n_frames;
    const char *keys[MAX_KEYS];
} Twin;

static const char *const DEBUG_WEIGHT_OPTS[] = {"debug", "true", "motion_fps_weight", "0.6", NULL};
static const char *const V2_WEIGHT_OPTS[] = {"motion_fps_weight", "0.3", NULL};
static const char *const V2_CAP_OPTS[] = {"motion_max_val", "4", NULL};
static const char *const FORCE_ZERO_OPTS[] = {"debug", "true", "motion_force_zero", "true", NULL};

static const Twin TWINS[] = {
    {"motion",
     "motion_cuda",
     NULL,
     2u,
     0u,
     {"VMAF_integer_feature_motion2_score", "VMAF_integer_feature_motion3_score", NULL}},
    {"motion_v2",
     "motion_v2_cuda",
     NULL,
     3u,
     0u,
     {"VMAF_integer_feature_motion_v2_sad_score", "VMAF_integer_feature_motion2_v2_score",
      "VMAF_integer_feature_motion3_v2_score"}},
    /* The debug score carries the fps weight on the CPU; mfw is a
     * FEATURE_PARAM, so every key gains the _mfw_0.6 suffix. */
    {"motion",
     "motion_cuda",
     DEBUG_WEIGHT_OPTS,
     3u,
     0u,
     {"integer_motion_mfw_0.6", "integer_motion2_mfw_0.6", "integer_motion3_mfw_0.6"}},
    /* The CPU motion_v2 publishes MIN(sad * motion_fps_weight, motion_max_val)
     * and derives motion2_v2 / motion3_v2 from that; mfw and mmxv are
     * FEATURE_PARAMs, so the keys carry them. */
    {"motion_v2",
     "motion_v2_cuda",
     V2_WEIGHT_OPTS,
     3u,
     0u,
     {"VMAF_integer_feature_motion_v2_sad_score_mfw_0.3",
      "VMAF_integer_feature_motion2_v2_score_mfw_0.3",
      "VMAF_integer_feature_motion3_v2_score_mfw_0.3"}},
    {"motion_v2",
     "motion_v2_cuda",
     V2_CAP_OPTS,
     3u,
     0u,
     {"VMAF_integer_feature_motion_v2_sad_score_mmxv_4",
      "VMAF_integer_feature_motion2_v2_score_mmxv_4",
      "VMAF_integer_feature_motion3_v2_score_mmxv_4"}},
    /* One frame: the CPU flush still emits motion2_v2 = motion3_v2 = 0. */
    {"motion_v2",
     "motion_v2_cuda",
     NULL,
     3u,
     1u,
     {"VMAF_integer_feature_motion_v2_sad_score", "VMAF_integer_feature_motion2_v2_score",
      "VMAF_integer_feature_motion3_v2_score"}},
    /* motion_force_zero: zeros on every frame, as the CPU reports. The twin's
     * init() swaps submit() / collect() for extract() here, and the engine
     * used to call the cleared submit() on the first frame (SIGSEGV on an
     * RTX 4090, 2026-09-30); force_0 is the option's FEATURE_PARAM alias. */
    {"motion",
     "motion_cuda",
     FORCE_ZERO_OPTS,
     3u,
     0u,
     {"integer_motion_force_0", "integer_motion2_force_0", "integer_motion3_force_0"}},
};

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

static unsigned twin_frames(const Twin *t)
{
    return t->n_frames ? t->n_frames : NUM_FRAMES;
}

/* Feed the twin's frame count, flush, and read every score of every frame. */
static char *score_sequence(VmafContext *vmaf, const char *feature, const Twin *t, Geometry g,
                            unsigned bpc, double out[NUM_FRAMES][MAX_KEYS])
{
    mu_assert("vmaf_use_feature failed", !use_feature(vmaf, feature, t->opts));
    for (unsigned i = 0; i < twin_frames(t); i++) {
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
    for (unsigned i = 0; i < twin_frames(t); i++) {
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

/* A fresh CUDA state per case. `*skipped` is set when there is no device.
 * The state is freed only after vmaf_close() (ADR-0157 ownership order). */
static char *score_cuda(const Twin *t, Geometry g, unsigned bpc, double out[NUM_FRAMES][MAX_KEYS],
                        int *skipped)
{
    VmafCudaState *state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    if (vmaf_cuda_state_init(&state, cuda_cfg) != 0 || state == NULL) {
        *skipped = 1;
        return NULL;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    char *msg = NULL;
    if (vmaf_init(&vmaf, cfg)) {
        msg = "CUDA: vmaf_init failed";
    } else if (vmaf_cuda_import_state(vmaf, state)) {
        msg = "CUDA: vmaf_cuda_import_state failed";
    } else {
        msg = score_sequence(vmaf, t->cuda, t, g, bpc, out);
    }
    if (vmaf && vmaf_close(vmaf) && !msg) {
        msg = "CUDA: vmaf_close failed";
    }
    if (vmaf_cuda_state_free(state) && !msg) {
        msg = "CUDA: vmaf_cuda_state_free failed";
    }
    return msg;
}

static char *check_case(const Twin *t, Geometry g, unsigned bpc, int *skipped)
{
    double cpu[NUM_FRAMES][MAX_KEYS];
    double gpu[NUM_FRAMES][MAX_KEYS];
    mu_assert_msg(score_cpu_scalar(t, g, bpc, cpu));
    mu_assert_msg(score_cuda(t, g, bpc, gpu, skipped));
    if (*skipped) {
        return NULL;
    }
    for (unsigned i = 0; i < twin_frames(t); i++) {
        for (unsigned k = 0; k < t->n_keys; k++) {
            if (cpu[i][k] != gpu[i][k]) {
                (void)fprintf(stderr, "\n  %s %ux%u %u-bit frame %u %s: cpu=%.17g cuda=%.17g\n",
                              t->cuda, g.w, g.h, bpc, i, t->keys[k], cpu[i][k], gpu[i][k]);
                return "CUDA motion differs from the scalar CPU";
            }
        }
    }
    return NULL;
}

static char *check_twin(const Twin *t, size_t n_geometries, size_t n_depths)
{
    for (size_t b = 0; b < n_depths; b++) {
        for (size_t i = 0; i < n_geometries; i++) {
            int skipped = 0;
            mu_assert_msg(check_case(t, ACCEPTED[i], BIT_DEPTHS[b], &skipped));
            if (skipped) {
                (void)fprintf(stderr, "[skip: no CUDA device] ");
                mu_skipped = 1;
                return NULL;
            }
        }
    }
    return NULL;
}

static char *test_motion_cuda_matches_scalar_cpu(void)
{
    return check_twin(&TWINS[0], NUM_ACCEPTED, NUM_BIT_DEPTHS);
}

static char *test_motion_v2_cuda_matches_scalar_cpu(void)
{
    return check_twin(&TWINS[1], NUM_ACCEPTED, NUM_BIT_DEPTHS);
}

/* The first seven geometries at 8 bits are enough to pin the weighted debug
 * score; the SAD itself is covered above. */
static char *test_motion_cuda_debug_score_is_weighted(void)
{
    return check_twin(&TWINS[2], 7u, 1u);
}

/* The weighted and the capped motion_v2 scores, and a one-frame run, on the
 * first seven geometries at 8 and 10 bits. */
static char *test_motion_v2_cuda_options_and_one_frame(void)
{
    for (size_t t = 3; t < 6; t++) {
        mu_assert_msg(check_twin(&TWINS[t], 7u, 2u));
        if (mu_skipped)
            return NULL;
    }
    return NULL;
}

/* motion_force_zero on the first three geometries at 8 bits: the scores are
 * constant, so the case pins the dispatch, not the arithmetic. */
static char *test_motion_cuda_force_zero(void)
{
    return check_twin(&TWINS[6], 3u, 1u);
}

/* init() with a zeroed private state. Rejected sizes return before init()
 * reads the CUDA state or the options, so neither a device nor a close is
 * needed. */
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

static char *test_motion_cuda_rejects_below_3x3(void)
{
    static const char *const twins[] = {"motion_cuda", "motion_v2_cuda"};
    for (size_t t = 0; t < sizeof(twins) / sizeof(twins[0]); t++) {
        VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(twins[t]);
        mu_assert("CUDA motion extractor is not registered", fex != NULL);
        for (size_t i = 0; i < NUM_REJECTED; i++) {
            if (!init_rejects(fex, REJECTED[i])) {
                (void)fprintf(stderr, "\n  %s accepted %ux%u\n", twins[t], REJECTED[i].w,
                              REJECTED[i].h);
                return "CUDA motion must reject frames below 3x3 with -EINVAL";
            }
        }
    }
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_motion_cuda_rejects_below_3x3),
        MU_TEST(test_motion_cuda_matches_scalar_cpu),
        MU_TEST(test_motion_v2_cuda_matches_scalar_cpu),
        MU_TEST(test_motion_cuda_debug_score_is_weighted),
        MU_TEST(test_motion_v2_cuda_options_and_one_frame),
        MU_TEST(test_motion_cuda_force_zero),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
