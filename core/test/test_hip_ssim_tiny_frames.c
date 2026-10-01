/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * integer_ssim_hip against the CPU `ssim` with ==, `enable_db` on, on frames
 * of at most ISSIM_HIP_RASTER_MAX_PIXELS pixels
 * (T-HIP-INTEGER-SSIM-TINY-IDENTICAL-DB-2026-09-30, ADR-1400).
 *
 * The CPU adds one term per pixel into a running double in raster order
 * (integer_ssim.c::calc_ssim()). On an identical window its quotient
 * `((w * f) * g) / (f * g)` is the weight `w` up to an ulp, and whether the
 * running sum absorbs that ulp depends on the frame: an identical flat 1x1
 * frame of 0 scores 1 - 2^-52 (156.54 dB), a flat 2x2 frame of 2 and a flat
 * 3x3 frame of 51 score 1 - 2^-53 (159.55 dB), and larger identical frames
 * mostly, but not always, exactly 1 (+inf). The twin's
 * per-block tree with the identical-window rule of ADR-1382 scored every
 * identical frame exactly 1. For frames up to the bound the device now
 * writes the CPU's quotient per pixel and collect() adds them in the CPU's
 * order, so every score equals the CPU's bit for bit, identical or not, at
 * every bit depth.
 *
 * Positive: 1x1 up to 64x64, strips and odd sizes, at 8, 10, 12 and 16 bits;
 * each run scores identical noise, an identical flat frame, an identical
 * low-variance frame and a distorted frame. Boundary: 64x64 is the last frame
 * on the raster path; 65x64 and 129x33 take the per-block tree, where the
 * distorted frame agrees within 1e-12 and the identical frames still report
 * the CPU's value. Negative: an empty frame is refused with -EINVAL, which
 * needs no device.
 *
 * The device cases skip (exit 77) when no HIP device is visible.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mu_table.h"
#include "test.h"

#include "feature/feature_extractor.h"
#include "feature/hip/integer_ssim_hip.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Frame kinds, one per frame index. */
enum {
    FRAME_IDENTICAL_NOISE = 0,
    FRAME_IDENTICAL_FLAT,
    FRAME_IDENTICAL_LOW_VARIANCE,
    FRAME_DISTORTED,
    NUM_FRAMES,
};

/* Linear tolerance of the per-block tree against the CPU's raster sum. */
#define TREE_TOLERANCE 1e-12

typedef struct {
    unsigned w;
    unsigned h;
} Geometry;

/* One run: the frame size, the bit depth and the sample value of the flat
 * frame. */
typedef struct {
    Geometry g;
    unsigned bpc;
    unsigned flat;
} Case;

/* All at most ISSIM_HIP_RASTER_MAX_PIXELS pixels. 2x37 and 10x5 are sizes
 * where an identical frame is not exactly 1 on the CPU for some contents. */
static const Geometry RASTER[] = {
    {1u, 1u},  {2u, 2u},  {1u, 2u},  {2u, 1u},  {3u, 3u},   {4u, 4u},   {10u, 5u},
    {2u, 37u}, {16u, 8u}, {17u, 9u}, {1u, 97u}, {131u, 1u}, {33u, 31u}, {64u, 64u},
};
#define NUM_RASTER (sizeof(RASTER) / sizeof(RASTER[0]))

/* Just above the bound: the per-block tree. */
static const Geometry TREE[] = {{65u, 64u}, {129u, 33u}};
#define NUM_TREE (sizeof(TREE) / sizeof(TREE[0]))

static const unsigned BIT_DEPTHS[] = {8u, 10u, 12u, 16u};
#define NUM_BIT_DEPTHS (sizeof(BIT_DEPTHS) / sizeof(BIT_DEPTHS[0]))

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

/* The reference luma sample of `frame` at (row, col). */
static unsigned ref_sample(const Case *c, unsigned row, unsigned col, unsigned frame)
{
    const uint32_t noise = sample_hash(row, col, frame);
    if (frame == FRAME_IDENTICAL_FLAT) {
        return c->flat;
    }
    if (frame == FRAME_IDENTICAL_LOW_VARIANCE) {
        return (1u << (c->bpc - 1u)) + (noise & 3u);
    }
    return noise >> (32u - c->bpc);
}

/* The distorted sample: the reference, except on FRAME_DISTORTED, where it
 * moves by -3..4 codes of the 8-bit scale and stays inside the range. */
static unsigned dist_sample(const Case *c, unsigned row, unsigned col, unsigned frame)
{
    const unsigned ref = ref_sample(c, row, col, frame);
    if (frame != FRAME_DISTORTED) {
        return ref;
    }
    const int max = (int)((1u << c->bpc) - 1u);
    const int step = (int)(sample_hash(col, row, frame + 7u) & 7u) - 3;
    const int moved = (int)ref + step * (int)(1u << (c->bpc - 8u));
    return (unsigned)(moved < 0 ? 0 : (moved > max ? max : moved));
}

static void fill_plane(VmafPicture *pic, const Case *c, unsigned p, unsigned frame, int distorted)
{
    for (unsigned row = 0; row < pic->h[p]; row++) {
        for (unsigned col = 0; col < pic->w[p]; col++) {
            unsigned v = 1u << (c->bpc - 1u);
            if (p == 0u) {
                v = distorted ? dist_sample(c, row, col, frame) : ref_sample(c, row, col, frame);
            }
            if (c->bpc == 8u) {
                ((uint8_t *)pic->data[p])[(row * pic->stride[p]) + col] = (uint8_t)v;
            } else {
                ((uint16_t *)pic->data[p])[(row * (pic->stride[p] / 2u)) + col] = (uint16_t)v;
            }
        }
    }
}

static int fill_picture(VmafPicture *pic, const Case *c, unsigned frame, int distorted)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, c->bpc, c->g.w, c->g.h);
    if (err) {
        return err;
    }
    for (unsigned p = 0; p < 3u; p++) {
        fill_plane(pic, c, p, frame, distorted);
    }
    return 0;
}

/* Feed the four frames of `c` and flush. */
static char *feed_frames(VmafContext *vmaf, const Case *c)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        mu_assert("reference allocation failed", !fill_picture(&ref, c, i, 0));
        if (fill_picture(&dist, c, i, 1)) {
            (void)vmaf_picture_unref(&ref);
            return "distorted allocation failed";
        }
        mu_assert("vmaf_read_pictures failed", !vmaf_read_pictures(vmaf, &ref, &dist, i));
    }
    mu_assert("vmaf_read_pictures(EOS) failed", !vmaf_read_pictures(vmaf, NULL, NULL, 0));
    return NULL;
}

/* Run `feature` with `enable_db` (or without) over the four frames and read
 * the `ssim` score of each. */
static char *score_sequence(VmafContext *vmaf, const char *feature, const Case *c, int enable_db,
                            double out[NUM_FRAMES])
{
    VmafFeatureDictionary *opts = NULL;
    if (enable_db) {
        mu_assert("option dictionary failed",
                  !vmaf_feature_dictionary_set(&opts, "enable_db", "true"));
    }
    mu_assert("vmaf_use_feature failed", !vmaf_use_feature(vmaf, feature, opts));
    mu_assert_msg(feed_frames(vmaf, c));
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        mu_assert("ssim score missing", !vmaf_feature_score_at_index(vmaf, "ssim", &out[i], i));
    }
    return NULL;
}

static char *score_cpu(const Case *c, int enable_db, double out[NUM_FRAMES])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("CPU: vmaf_init failed", !vmaf_init(&vmaf, cfg));
    char *msg = score_sequence(vmaf, "ssim", c, enable_db, out);
    (void)vmaf_close(vmaf);
    return msg;
}

/* A fresh HIP state per case. `*skipped` is set when there is no device. */
static char *score_hip(const Case *c, int enable_db, double out[NUM_FRAMES], int *skipped)
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
        msg = score_sequence(vmaf, "integer_ssim_hip", c, enable_db, out);
    }
    if (vmaf) {
        (void)vmaf_close(vmaf);
    }
    vmaf_hip_state_free(&state);
    return msg;
}

/* Score `c` on both sides. Returns NULL with *skipped set (and mu_skipped)
 * when there is no device. */
static char *score_both(const Case *c, int enable_db, double cpu[NUM_FRAMES],
                        double gpu[NUM_FRAMES], int *skipped)
{
    mu_assert_msg(score_cpu(c, enable_db, cpu));
    mu_assert_msg(score_hip(c, enable_db, gpu, skipped));
    if (*skipped) {
        (void)fprintf(stderr, "[skip: no HIP device] ");
        mu_skipped = 1;
    }
    return NULL;
}

/* Every frame of the raster path equals the CPU's dB score exactly. */
static char *check_raster_case(const Case *c, int *skipped)
{
    double cpu[NUM_FRAMES];
    double gpu[NUM_FRAMES];
    mu_assert("fixture exceeds the raster bound", c->g.w * c->g.h <= ISSIM_HIP_RASTER_MAX_PIXELS);
    mu_assert_msg(score_both(c, 1, cpu, gpu, skipped));
    for (unsigned i = 0; !*skipped && i < NUM_FRAMES; i++) {
        if (cpu[i] != gpu[i]) {
            (void)fprintf(stderr, "\n  %ux%u %u-bit frame %u: cpu=%.17g dB hip=%.17g dB\n", c->g.w,
                          c->g.h, c->bpc, i, cpu[i], gpu[i]);
            return "integer_ssim_hip differs from the CPU on a raster-path frame";
        }
    }
    return NULL;
}

static char *test_tiny_frames_equal_cpu_db(void)
{
    for (size_t b = 0; b < NUM_BIT_DEPTHS; b++) {
        for (size_t i = 0; i < NUM_RASTER; i++) {
            const Case c = {RASTER[i], BIT_DEPTHS[b], 1u << (BIT_DEPTHS[b] - 1u)};
            int skipped = 0;
            mu_assert_msg(check_raster_case(&c, &skipped));
            if (skipped) {
                return NULL;
            }
        }
    }
    return NULL;
}

/* Identical flat frames on which the CPU's sum keeps its ulp: 156.54 dB and
 * 159.55 dB, not +inf. The first two are the sizes the row names. */
static char *test_identical_flat_frames_report_cpu_db(void)
{
    static const Case flat[] = {
        {{1u, 1u}, 8u, 0u},
        {{2u, 2u}, 8u, 2u},
        {{3u, 3u}, 8u, 51u},
    };
    for (size_t i = 0; i < sizeof(flat) / sizeof(flat[0]); i++) {
        double cpu[NUM_FRAMES];
        double gpu[NUM_FRAMES];
        int skipped = 0;
        mu_assert_msg(score_both(&flat[i], 1, cpu, gpu, &skipped));
        if (skipped) {
            return NULL;
        }
        if (!isfinite(cpu[FRAME_IDENTICAL_FLAT]) || cpu[FRAME_IDENTICAL_FLAT] < 150.0) {
            (void)fprintf(stderr, "\n  %ux%u flat %u: cpu=%.17g dB\n", flat[i].g.w, flat[i].g.h,
                          flat[i].flat, cpu[FRAME_IDENTICAL_FLAT]);
            return "the CPU no longer reports a finite dB on this identical flat frame";
        }
        if (gpu[FRAME_IDENTICAL_FLAT] != cpu[FRAME_IDENTICAL_FLAT]) {
            (void)fprintf(stderr, "\n  %ux%u flat %u: cpu=%.17g dB hip=%.17g dB\n", flat[i].g.w,
                          flat[i].g.h, flat[i].flat, cpu[FRAME_IDENTICAL_FLAT],
                          gpu[FRAME_IDENTICAL_FLAT]);
            return "integer_ssim_hip differs from the CPU on an identical flat tiny frame";
        }
    }
    return NULL;
}

/* Above the bound the per-block tree runs: linear scores within
 * TREE_TOLERANCE, and the identical frames exactly the CPU's. */
static char *check_tree_case(const Case *c, int *skipped)
{
    double cpu[NUM_FRAMES];
    double gpu[NUM_FRAMES];
    mu_assert("fixture is not above the raster bound",
              c->g.w * c->g.h > ISSIM_HIP_RASTER_MAX_PIXELS);
    mu_assert_msg(score_both(c, 0, cpu, gpu, skipped));
    for (unsigned i = 0; !*skipped && i < NUM_FRAMES; i++) {
        const double limit = (i == FRAME_DISTORTED) ? TREE_TOLERANCE : 0.0;
        if (!(fabs(cpu[i] - gpu[i]) <= limit)) {
            (void)fprintf(stderr, "\n  %ux%u %u-bit frame %u: cpu=%.17g hip=%.17g\n", c->g.w,
                          c->g.h, c->bpc, i, cpu[i], gpu[i]);
            return "integer_ssim_hip differs from the CPU above the raster bound";
        }
    }
    return NULL;
}

static char *test_frames_above_bound_keep_tree_parity(void)
{
    for (size_t b = 0; b < NUM_BIT_DEPTHS; b++) {
        for (size_t i = 0; i < NUM_TREE; i++) {
            const Case c = {TREE[i], BIT_DEPTHS[b], 1u << (BIT_DEPTHS[b] - 1u)};
            int skipped = 0;
            mu_assert_msg(check_tree_case(&c, &skipped));
            if (skipped) {
                return NULL;
            }
        }
    }
    return NULL;
}

/* init() refuses an empty frame before it touches the device. */
static char *test_empty_frame_is_rejected(void)
{
    static const Geometry empty[] = {{0u, 4u}, {4u, 0u}, {0u, 0u}};
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("integer_ssim_hip");
    mu_assert("integer_ssim_hip is not registered", fex != NULL);
    for (size_t i = 0; i < 3u; i++) {
        void *priv = calloc(1, fex->priv_size);
        mu_assert("calloc failed", priv != NULL);
        fex->priv = priv;
        const int rc = fex->init(fex, VMAF_PIX_FMT_YUV420P, 8u, empty[i].w, empty[i].h);
        free(priv);
        fex->priv = NULL;
        mu_assert("integer_ssim_hip must refuse an empty frame with -EINVAL", rc == -EINVAL);
    }
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_empty_frame_is_rejected),
        MU_TEST(test_identical_flat_frames_report_cpu_db),
        MU_TEST(test_tiny_frames_equal_cpu_db),
        MU_TEST(test_frames_above_bound_keep_tree_parity),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
