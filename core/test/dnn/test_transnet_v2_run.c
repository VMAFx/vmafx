/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  End-to-end tests of the transnet_v2 extractor with the shipped model
 *  (ADR-1527): the rank-5 input opens through the DNN session path, the one
 *  output binds by position, and the upstream 100-frame windows mark the
 *  last frame of each shot of a synthetic clip, including the windows the
 *  flush runs. Linked against libvmaf so the public surface is exercised.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "float_bits.h"

#include "libvmaf/dnn.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

/* Path is relative to workdir = project root (set in meson.build). */
#define TRANSNET_MODEL "model/tiny/transnet_v2.onnx"
#define CLIP_W 96u
#define CLIP_H 54u
#define MAX_CUTS 2u

/* Three synthetic shots: a slowly moving ramp, a flickering checkerboard and
 * a second ramp. The shipped model gives every cut frame p > 0.99 and every
 * other frame p < 0.16 on these clips (upstream predict_frames() in Python
 * on the same thumbnails gives the same numbers). */
static unsigned shot_sample(unsigned shot, unsigned x, unsigned y, unsigned t)
{
    switch (shot % 3u) {
    case 0u:
        return (x * 2u + y * 3u + t) % 160u + 40u;
    case 1u:
        return (((x / 8u + y / 8u) % 2u) * 150u) + 50u + (t % 8u);
    default:
        return 200u - (x + 2u * y + t) / 2u;
    }
}

typedef struct Clip {
    unsigned n_frames;
    unsigned n_cuts;
    unsigned cuts[MAX_CUTS]; /* first frame of each new shot */
    unsigned bpc;
} Clip;

static unsigned shot_of(const Clip *clip, unsigned t)
{
    unsigned shot = 0u;
    for (unsigned i = 0; i < clip->n_cuts; ++i)
        shot += t >= clip->cuts[i] ? 1u : 0u;
    return shot;
}

static void fill_plane(VmafPicture *pic, unsigned p, const Clip *clip, unsigned t)
{
    const unsigned shift = clip->bpc - 8u;
    const unsigned shot = shot_of(clip, t);
    for (unsigned y = 0; y < pic->h[p]; ++y) {
        uint8_t *row8 = (uint8_t *)pic->data[p] + (size_t)y * pic->stride[p];
        uint16_t *row16 = (uint16_t *)pic->data[p] + (size_t)y * (pic->stride[p] / 2u);
        for (unsigned x = 0; x < pic->w[p]; ++x) {
            const unsigned v = p == 0u ? shot_sample(shot, x, y, t) : 128u;
            if (clip->bpc == 8u) {
                row8[x] = (uint8_t)v;
            } else {
                row16[x] = (uint16_t)(v << shift);
            }
        }
    }
}

static int read_frame(VmafContext *ctx, const Clip *clip, unsigned t, unsigned index)
{
    VmafPicture ref = {0};
    VmafPicture dist = {0};
    int rc = vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV420P, clip->bpc, CLIP_W, CLIP_H);
    if (rc)
        return rc;
    rc = vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV420P, clip->bpc, CLIP_W, CLIP_H);
    if (rc) {
        (void)vmaf_picture_unref(&ref);
        return rc;
    }
    for (unsigned p = 0; p < 3u; ++p) {
        fill_plane(&ref, p, clip, t);
        fill_plane(&dist, p, clip, t);
    }
    return vmaf_read_pictures(ctx, &ref, &dist, index);
}

static VmafContext *open_transnet_ctx(void)
{
    VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_NONE,
        .n_threads = 1,
        .n_subsample = 1,
    };
    VmafContext *ctx = NULL;
    if (vmaf_init(&ctx, cfg) < 0)
        return NULL;
    VmafFeatureDictionary *opts = NULL;
    if (vmaf_feature_dictionary_set(&opts, "model_path", TRANSNET_MODEL) != 0 ||
        vmaf_use_feature(ctx, "transnet_v2", opts) != 0) {
        (void)vmaf_close(ctx);
        return NULL;
    }
    return ctx;
}

/* Read the clip, flush, and require shot_boundary == 1 exactly on the last
 * frame of every shot but the clip's last, with a probability on every
 * frame. */
static char *expect_cut_flags(const Clip *clip)
{
    VmafContext *ctx = open_transnet_ctx();
    mu_assert("transnet_v2 registered", ctx != NULL);
    int rc = 0;
    for (unsigned t = 0; t < clip->n_frames && rc == 0; ++t)
        rc = read_frame(ctx, clip, t, t);
    mu_assert("ADR-1527: every frame read (the rank-5 model opens and runs)", rc == 0);
    mu_assert("flush", vmaf_read_pictures(ctx, NULL, NULL, 0u) == 0);
    unsigned wrong = 0u;
    unsigned missing = 0u;
    for (unsigned t = 0; t < clip->n_frames; ++t) {
        double flag = -1.0;
        double prob = -1.0;
        missing += vmaf_feature_score_at_index(ctx, "shot_boundary", &flag, t) != 0;
        missing += vmaf_feature_score_at_index(ctx, "shot_boundary_probability", &prob, t) != 0;
        const double want = (shot_of(clip, t + 1u) != shot_of(clip, t)) ? 1.0 : 0.0;
        wrong += vmaf_test_identical_f64(flag, want) ? 0u : 1u;
    }
    (void)vmaf_close(ctx);
    mu_assert("ADR-1527: every frame carries both shot-boundary features", missing == 0u);
    mu_assert("ADR-1527: shot_boundary marks exactly the last frame of each shot", wrong == 0u);
    return NULL;
}

/* Positive: one cut, one window, run at flush. */
static char *test_transnet_v2_marks_cut(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    const Clip clip = {.n_frames = 30u, .n_cuts = 1u, .cuts = {15u}, .bpc = 8u};
    return expect_cut_flags(&clip);
}

/* Boundary: 60 frames leave two windows to the flush; the cut sits in the
 * second. */
static char *test_transnet_v2_flush_runs_two_windows(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    const Clip clip = {.n_frames = 60u, .n_cuts = 1u, .cuts = {52u}, .bpc = 8u};
    return expect_cut_flags(&clip);
}

/* Boundary: 130 frames run two windows while frames are read and one at
 * flush; cuts in the first and the last. */
static char *test_transnet_v2_streams_windows(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    const Clip clip = {.n_frames = 130u, .n_cuts = 2u, .cuts = {40u, 100u}, .bpc = 8u};
    return expect_cut_flags(&clip);
}

/* Negative: a clip without a cut marks no frame. */
static char *test_transnet_v2_no_cut_marks_nothing(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    const Clip clip = {.n_frames = 130u, .n_cuts = 0u, .cuts = {0u}, .bpc = 8u};
    return expect_cut_flags(&clip);
}

/* Boundary: 10-bit samples are scaled to the 0..255 range the model takes. */
static char *test_transnet_v2_marks_cut_10bit(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    const Clip clip = {.n_frames = 30u, .n_cuts = 1u, .cuts = {15u}, .bpc = 10u};
    return expect_cut_flags(&clip);
}

/* Negative: the windows are kept by frame index, so a skipped index fails. */
static char *test_transnet_v2_rejects_index_gap(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    const Clip clip = {.n_frames = 4u, .n_cuts = 0u, .cuts = {0u}, .bpc = 8u};
    VmafContext *ctx = open_transnet_ctx();
    mu_assert("transnet_v2 registered", ctx != NULL);
    int rc = read_frame(ctx, &clip, 0u, 0u);
    if (rc == 0)
        rc = read_frame(ctx, &clip, 1u, 1u);
    const int gap_rc = rc == 0 ? read_frame(ctx, &clip, 3u, 3u) : rc;
    (void)vmaf_close(ctx);
    mu_assert("frames 0 and 1 read", rc == 0);
    mu_assert("ADR-1527: a skipped frame index is refused", gap_rc == -EINVAL);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_transnet_v2_marks_cut),       MU_TEST(test_transnet_v2_flush_runs_two_windows),
        MU_TEST(test_transnet_v2_streams_windows), MU_TEST(test_transnet_v2_no_cut_marks_nothing),
        MU_TEST(test_transnet_v2_marks_cut_10bit), MU_TEST(test_transnet_v2_rejects_index_gap),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
