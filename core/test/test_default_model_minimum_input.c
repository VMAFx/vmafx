/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Default model vmaf_v1.0.16_3d0h minimum input dimensions gate.
 *
 * The default model reads CAMBI, which needs width or height >= 216
 * (CAMBI_MIN_WIDTH_HEIGHT), and speed_chroma, which needs both chroma
 * dimensions >= 80 (SPEED_INTERNAL_MIN_DIMENSION). An input below either limit
 * must be refused by the library; 216x160 and 160x216 must be scored.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr`, and this
 * file mirrors the C spelling of the surface it exercises. ADR-1138. */

enum { TEST_FRAME_COUNT = 3u };

typedef struct RunResult {
    int read_err;
    int flush_err;
    int score_err;
    double score;
} RunResult;

static int allocate_and_fill_picture(VmafPicture *pic, unsigned w, unsigned h, unsigned seed)
{
    const int alloc_err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8u, w, h);
    if (alloc_err)
        return alloc_err;

    for (unsigned p = 0; p < 3u; p++) {
        uint8_t *row = (uint8_t *)pic->data[p];
        const unsigned plane_h = pic->h[p];
        const unsigned plane_w = pic->w[p];
        const ptrdiff_t stride = pic->stride[p];

        for (unsigned y = 0; y < plane_h; y++) {
            for (unsigned x = 0; x < plane_w; x++) {
                // SAFETY: row points into plane p buffer allocated by vmaf_picture_alloc.
                row[x] = (uint8_t)((x * 17u + y * 31u + seed * 101u + p * 37u) & 0xffu);
            }
            // SAFETY: stride is the allocated row pitch; row advances to the next valid scanline.
            row += stride;
        }
    }
    return 0;
}

static int feed_test_frames(VmafContext *ctx, unsigned w, unsigned h, int *first_read_err)
{
    *first_read_err = 0;
    for (unsigned i = 0; i < TEST_FRAME_COUNT; i++) {
        VmafPicture ref;
        VmafPicture dist;
        const int ref_err = allocate_and_fill_picture(&ref, w, h, 1u + i);
        if (ref_err)
            return ref_err;
        const int dist_err = allocate_and_fill_picture(&dist, w, h, 42u + i);
        if (dist_err) {
            const int unref_err = vmaf_picture_unref(&ref);
            if (unref_err)
                return unref_err;
            return dist_err;
        }
        /* vmaf_read_pictures takes ownership of both pictures on every return (ADR-1431). */
        const int read_err = vmaf_read_pictures(ctx, &ref, &dist, i);
        if (read_err) {
            *first_read_err = read_err;
            return 0;
        }
    }
    return 0;
}

static int execute_run(unsigned w, unsigned h, RunResult *res)
{
    res->read_err = 0;
    res->flush_err = 0;
    res->score_err = 0;
    res->score = 0.0;

    VmafContext *ctx = NULL;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .n_threads = 0};
    const int init_err = vmaf_init(&ctx, cfg);
    if (init_err)
        return init_err;

    VmafModel *model = NULL;
    VmafModelConfig mcfg = {0};
    const int load_err = vmaf_model_load(&model, &mcfg, "vmaf_v1.0.16_3d0h");
    if (load_err) {
        const int close_err = vmaf_close(ctx);
        return close_err ? close_err : load_err;
    }

    const int use_err = vmaf_use_features_from_model(ctx, model);
    if (use_err) {
        vmaf_model_destroy(model);
        const int close_err = vmaf_close(ctx);
        return close_err ? close_err : use_err;
    }

    const int feed_err = feed_test_frames(ctx, w, h, &res->read_err);
    if (feed_err) {
        vmaf_model_destroy(model);
        const int close_err = vmaf_close(ctx);
        return close_err ? close_err : feed_err;
    }

    res->flush_err = vmaf_read_pictures(ctx, NULL, NULL, 0u);
    res->score_err = vmaf_score_pooled(ctx, model, VMAF_POOL_METHOD_MEAN, &res->score, 0u,
                                       TEST_FRAME_COUNT - 1u);

    vmaf_model_destroy(model);
    const int close_err = vmaf_close(ctx);
    return close_err;
}

/* Refused: the first read returns -EINVAL and no pooled score follows. */
static char *assert_refused(unsigned w, unsigned h)
{
    RunResult res;
    const int rc = execute_run(w, h, &res);
    mu_assert("execute_run failed unexpectedly", rc == 0);
    mu_assert("read_pictures must fail with -EINVAL", res.read_err == -EINVAL);
    mu_assert("a refused input must not produce a pooled score",
              res.flush_err != 0 || res.score_err != 0);
    return NULL;
}

/* Accepted: every frame is read and the pooled score is finite. */
static char *assert_accepted(unsigned w, unsigned h)
{
    RunResult res;
    const int rc = execute_run(w, h, &res);
    mu_assert("execute_run failed unexpectedly", rc == 0);
    mu_assert("read_pictures must succeed", res.read_err == 0);
    mu_assert("flush must succeed", res.flush_err == 0);
    mu_assert("score_pooled must succeed", res.score_err == 0);
    mu_assert("score must be finite", isfinite(res.score));
    return NULL;
}

static char *test_case_a_160x120_refused(void)
{
    /* Both dimensions below 216 (CAMBI) and chroma 80x60 below 80 (SpEED). */
    return assert_refused(160u, 120u);
}

static char *test_case_b_216x160_accepted(void)
{
    return assert_accepted(216u, 160u);
}

static char *test_case_c_boundary_dimensions(void)
{
    char *msg = assert_refused(215u, 215u);
    if (msg)
        return msg;
    return assert_accepted(160u, 216u);
}

static char *test_case_d_speed_chroma_limit(void)
{
    /* 216 satisfies CAMBI; chroma 108x79 is below the SpEED limit of 80. */
    return assert_refused(216u, 158u);
}

char *run_tests(void)
{
    mu_run_test(test_case_a_160x120_refused);
    mu_run_test(test_case_b_216x160_accepted);
    mu_run_test(test_case_c_boundary_dimensions);
    mu_run_test(test_case_d_speed_chroma_limit);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
