/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * A model collection's score can be read more than once
 * (T-MODEL-SET-SCORE-NOT-IDEMPOTENT-2026-10-05).
 *
 * vmaf_score_at_index_model_collection() predicts every member model and
 * writes the members' scores and four named bootstrap scores of the frame
 * into the feature collector, which refuses a second write of a frame. On
 * master before the fix a second per-frame call of a frame, or
 * vmaf_score_pooled_model_collection() over a range holding a frame already
 * scored per frame, failed with -EINVAL ("feature ... cannot be overwritten").
 * A single model reads its stored score first (vmaf_score_at_index()); a
 * collection now does the same.
 *
 * Failing first: without the fix, test_second_frame_score and
 * test_pooled_after_frame_score fail (measured on master 782eba01f).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"
#include "mu_table.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

enum { W = 176, H = 144, N_FRAMES = 5 };

typedef struct Session {
    VmafContext *vmaf;
    VmafModel *lead;
    VmafModelCollection *collection;
} Session;

static int fill_picture(VmafPicture *pic, unsigned seed)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, W, H);
    if (err)
        return err;
    for (unsigned p = 0; p < 3u; p++) {
        uint8_t *row = pic->data[p];
        for (unsigned y = 0; y < pic->h[p]; y++, row += pic->stride[p]) {
            for (unsigned x = 0; x < pic->w[p]; x++)
                row[x] = (uint8_t)((x * 7u + y * 13u + seed * 29u + p * 3u) & 0xffu);
        }
    }
    return 0;
}

/* vmaf_b_v0.6.3 over N_FRAMES generated frames, flushed. */
static bool open_session(Session *s)
{
    memset(s, 0, sizeof(*s));
    VmafConfiguration cfg;
    memset(&cfg, 0, sizeof(cfg));
    VmafModelConfig mcfg = {0};
    if (vmaf_init(&s->vmaf, cfg) ||
        vmaf_model_collection_load(&s->lead, &s->collection, &mcfg, "vmaf_b_v0.6.3") ||
        vmaf_use_features_from_model_collection(s->vmaf, s->collection))
        return false;
    for (unsigned i = 0; i < N_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        if (fill_picture(&ref, i))
            return false;
        if (fill_picture(&dist, i + 50u)) {
            (void)vmaf_picture_unref(&ref);
            return false;
        }
        if (vmaf_read_pictures(s->vmaf, &ref, &dist, i))
            return false;
    }
    return vmaf_read_pictures(s->vmaf, NULL, NULL, 0) == 0;
}

static bool close_session(Session *s)
{
    const bool ok = vmaf_close(s->vmaf) == 0;
    vmaf_model_destroy(s->lead);
    vmaf_model_collection_destroy(s->collection);
    return ok;
}

static bool same_bits(double a, double b)
{
    uint64_t x = 0;
    uint64_t y = 0;
    memcpy(&x, &a, sizeof(x));
    memcpy(&y, &b, sizeof(y));
    return x == y;
}

static bool same_score(const VmafModelCollectionScore *a, const VmafModelCollectionScore *b)
{
    return a->type == b->type &&
           same_bits(a->bootstrap.bagging_score, b->bootstrap.bagging_score) &&
           same_bits(a->bootstrap.stddev, b->bootstrap.stddev) &&
           same_bits(a->bootstrap.ci.p95.lo, b->bootstrap.ci.p95.lo) &&
           same_bits(a->bootstrap.ci.p95.hi, b->bootstrap.ci.p95.hi);
}

static char *test_second_frame_score(void)
{
    Session s;
    mu_assert("session", open_session(&s));
    VmafModelCollectionScore first;
    VmafModelCollectionScore second;
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    mu_assert("first", vmaf_score_at_index_model_collection(s.vmaf, s.collection, &first, 2) == 0);
    mu_assert("second",
              vmaf_score_at_index_model_collection(s.vmaf, s.collection, &second, 2) == 0);
    mu_assert("the same score",
              same_score(&first, &second) && first.type == VMAF_MODEL_COLLECTION_SCORE_BOOTSTRAP);
    mu_assert("close", close_session(&s));
    return NULL;
}

/* The pooled score of a fresh session, for comparison. */
static bool fresh_pooled(VmafModelCollectionScore *score)
{
    Session s;
    const bool ok = open_session(&s) &&
                    vmaf_score_pooled_model_collection(s.vmaf, s.collection, VMAF_POOL_METHOD_MEAN,
                                                       score, 0, N_FRAMES - 1) == 0;
    return close_session(&s) && ok;
}

static char *test_pooled_after_frame_score(void)
{
    VmafModelCollectionScore expected;
    memset(&expected, 0, sizeof(expected));
    mu_assert("fresh pooled score", fresh_pooled(&expected));
    Session s;
    mu_assert("session", open_session(&s));
    VmafModelCollectionScore frame;
    VmafModelCollectionScore pooled;
    memset(&frame, 0, sizeof(frame));
    memset(&pooled, 0, sizeof(pooled));
    mu_assert("frame 2",
              vmaf_score_at_index_model_collection(s.vmaf, s.collection, &frame, 2) == 0);
    mu_assert("pooled over frame 2",
              vmaf_score_pooled_model_collection(s.vmaf, s.collection, VMAF_POOL_METHOD_MEAN,
                                                 &pooled, 0, N_FRAMES - 1) == 0);
    mu_assert("equals the fresh session's", same_score(&pooled, &expected));
    mu_assert("pooled twice",
              vmaf_score_pooled_model_collection(s.vmaf, s.collection, VMAF_POOL_METHOD_MEAN,
                                                 &pooled, 0, N_FRAMES - 1) == 0 &&
                  same_score(&pooled, &expected));
    mu_assert("close", close_session(&s));
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_second_frame_score),
        MU_TEST(test_pooled_after_frame_score),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
