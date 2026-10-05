/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Synchronous scores of the VMAFx API (ADR-1852, RC4 WP2): per frame and
 * pooled, for features, models and model sets, against the libvmaf calls on
 * the same frames in a separate session, bit for bit; VMAFX_PENDING
 * where libvmaf returns -EAGAIN, without an error; every refusal named.
 *
 * Failing first: the functions do not exist on the WP1 base. Measured with
 * planted defects on this branch: a pool enum passed off by one fails
 * test_pooled_matches_libvmaf; PENDING turned into an error fails
 * test_pending_is_not_an_error.
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"
#include "mu_table.h"
#include "test.h"
#include "vmafx/vmafx.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

enum { W = 176, H = 144, N_FRAMES = 5 };

static const uint32_t all_pools[] = {
    VMAFX_POOL_MIN,    VMAFX_POOL_MAX,   VMAFX_POOL_MEAN,   VMAFX_POOL_HARMONIC_MEAN,
    VMAFX_POOL_MEDIAN, VMAFX_POOL_PERC5, VMAFX_POOL_PERC10, VMAFX_POOL_PERC20,
};

typedef struct Session {
    VmafxContext *context;
    VmafxModel *model;
    VmafxModelSet *set;
} Session;

static bool same_bits(double a, double b)
{
    return vt_same_bits(a, b);
}

static bool submit_frames(VmafxContext *context, unsigned n)
{
    const VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    uint8_t *data = malloc(vt_frame_bytes(&desc));
    bool ok = data != NULL;
    for (unsigned i = 0; i < n && ok; i++) {
        vt_fill(&desc, data, i);
        VmafxFrame *ref = vt_copy_frame(&desc, data);
        vt_fill(&desc, data, i + 50u);
        VmafxFrame *dist = vt_copy_frame(&desc, data);
        ok = vmafx_submit(context, ref, dist, i, NULL) == VMAFX_OK;
    }
    free(data);
    return ok;
}

/* A session scoring vmaf_v0.6.1 and the vmaf_b_v0.6.3 set over N_FRAMES. */
static bool open_session(Session *s, bool flush)
{
    memset(s, 0, sizeof(*s));
    return vmafx_context_create(NULL, &s->context, NULL) == VMAFX_OK &&
           vmafx_model_load(NULL, "vmaf_v0.6.1", &s->model, NULL) == VMAFX_OK &&
           vmafx_model_set_load(NULL, "vmaf_b_v0.6.3", &s->set, NULL) == VMAFX_OK &&
           vmafx_context_use_model(s->context, s->model, NULL) == VMAFX_OK &&
           vmafx_context_use_model_set(s->context, s->set, NULL) == VMAFX_OK &&
           submit_frames(s->context, N_FRAMES) &&
           (!flush || vmafx_flush(s->context, NULL) == VMAFX_OK);
}

static bool close_session(Session *s)
{
    const bool ok = vmafx_context_destroy(s->context, NULL) == VMAFX_OK;
    vmafx_model_unref(s->model);
    vmafx_model_set_unref(s->set);
    return ok;
}

/* The same frames and models through libvmaf in a separate session. */
typedef struct Legacy {
    VmafContext *vmaf;
    VmafModel *model;
    VmafModel *lead;
    VmafModelCollection *set;
} Legacy;

static bool read_legacy_frame(VmafContext *vmaf, uint8_t *data, unsigned i)
{
    const VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    VmafPicture pics[2];
    for (unsigned k = 0; k < 2u; k++) {
        if (vmaf_picture_alloc(&pics[k], VMAF_PIX_FMT_YUV420P, 8, W, H)) {
            return false;
        }
        vt_fill(&desc, data, k == 0u ? i : i + 50u);
        const uint8_t *src = data;
        for (unsigned p = 0; p < 3u; p++) {
            uint8_t *dst = pics[k].data[p];
            for (unsigned y = 0; y < pics[k].h[p]; y++, src += pics[k].w[p]) {
                memcpy(dst + (size_t)y * (size_t)pics[k].stride[p], src, pics[k].w[p]);
            }
        }
    }
    return vmaf_read_pictures(vmaf, &pics[0], &pics[1], i) == 0;
}

static bool open_legacy(Legacy *l)
{
    memset(l, 0, sizeof(*l));
    VmafConfiguration cfg;
    memset(&cfg, 0, sizeof(cfg));
    VmafModelConfig mcfg = {0};
    if (vmaf_init(&l->vmaf, cfg) || vmaf_model_load(&l->model, &mcfg, "vmaf_v0.6.1") ||
        vmaf_model_collection_load(&l->lead, &l->set, &mcfg, "vmaf_b_v0.6.3") ||
        vmaf_use_features_from_model(l->vmaf, l->model) ||
        vmaf_use_features_from_model_collection(l->vmaf, l->set)) {
        return false;
    }
    const VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    uint8_t *data = malloc(vt_frame_bytes(&desc));
    bool ok = data != NULL;
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        ok = read_legacy_frame(l->vmaf, data, i);
    }
    free(data);
    return ok && vmaf_read_pictures(l->vmaf, NULL, NULL, 0) == 0;
}

static bool close_legacy(Legacy *l)
{
    const bool ok = vmaf_close(l->vmaf) == 0;
    vmaf_model_destroy(l->model);
    vmaf_model_destroy(l->lead);
    vmaf_model_collection_destroy(l->set);
    return ok;
}

/* ---- Per frame -------------------------------------------------------------------------- */

static char *test_frame_scores_match_libvmaf(void)
{
    Session s;
    Legacy l;
    mu_assert("sessions", open_session(&s, true) && open_legacy(&l));
    for (unsigned i = 0; i < N_FRAMES; i++) {
        VmafxScore score = VMAFX_SCORE_INIT;
        double legacy = 0.0;
        mu_assert("model score",
                  vmafx_score_frame(s.context, s.model, i, &score, NULL) == VMAFX_OK &&
                      vmaf_score_at_index(l.vmaf, l.model, &legacy, i) == 0);
        mu_assert("bit-identical", same_bits(score.value, legacy) && score.index == i &&
                                       !strcmp(score.feature, "vmaf") && !score.extractor);
    }
    mu_assert("close", close_session(&s) && close_legacy(&l));
    return NULL;
}

static bool same_set_score(const VmafxModelSetScore *a, const VmafModelCollectionScore *b)
{
    return same_bits(a->bagging, b->bootstrap.bagging_score) &&
           same_bits(a->stddev, b->bootstrap.stddev) &&
           same_bits(a->ci95_lo, b->bootstrap.ci.p95.lo) &&
           same_bits(a->ci95_hi, b->bootstrap.ci.p95.hi);
}

/* A model set's per-frame and pooled scores each in a fresh session: the
 * engine's set prediction writes its member scores once per frame, so a
 * pooled call after a per-frame call of the same frame fails in libvmaf too
 * (docs/state.md T-MODEL-SET-SCORE-NOT-IDEMPOTENT-2026-10-05; fixed on master by
 * PR #2206, after which both may share one session). */
static char *test_set_frame_score(void)
{
    Session s;
    Legacy l;
    mu_assert("sessions", open_session(&s, true) && open_legacy(&l));
    VmafxModelSetScore score = VMAFX_MODEL_SET_SCORE_INIT;
    VmafModelCollectionScore legacy;
    mu_assert("frame", vmafx_score_frame_model_set(s.context, s.set, 2, &score, NULL) == VMAFX_OK &&
                           vmaf_score_at_index_model_collection(l.vmaf, l.set, &legacy, 2) == 0);
    mu_assert("per frame", score.pool == VMAFX_POOL_NONE && score.first == 2 && score.last == 2 &&
                               same_set_score(&score, &legacy));
    mu_assert("named", score.name && !strcmp(score.name, "vmaf"));
    mu_assert("close", close_session(&s) && close_legacy(&l));
    return NULL;
}

static char *test_set_pooled_score(void)
{
    Session s;
    Legacy l;
    mu_assert("sessions", open_session(&s, true) && open_legacy(&l));
    VmafxModelSetScore score = VMAFX_MODEL_SET_SCORE_INIT;
    VmafModelCollectionScore legacy;
    mu_assert("pooled", vmafx_score_pooled_model_set(s.context, s.set, VMAFX_POOL_MEAN, 0,
                                                     N_FRAMES - 1, &score, NULL) == VMAFX_OK &&
                            vmaf_score_pooled_model_collection(l.vmaf, l.set, VMAF_POOL_METHOD_MEAN,
                                                               &legacy, 0, N_FRAMES - 1) == 0);
    mu_assert("bit-identical", same_set_score(&score, &legacy) && score.pool == VMAFX_POOL_MEAN &&
                                   score.first == 0 && score.last == N_FRAMES - 1);
    mu_assert("close", close_session(&s) && close_legacy(&l));
    return NULL;
}

/* ---- Pooled -------------------------------------------------------------------------------- */

static bool pooled_case(Session *s, Legacy *l, uint32_t pool)
{
    VmafxPooledScore pooled = VMAFX_POOLED_SCORE_INIT;
    double legacy = 0.0;
    const enum VmafPoolingMethod method = (enum VmafPoolingMethod)pool;
    const bool model =
        vmafx_score_pooled(s->context, s->model, pool, 0, N_FRAMES - 1, &pooled, NULL) ==
            VMAFX_OK &&
        vmaf_score_pooled(l->vmaf, l->model, method, &legacy, 0, N_FRAMES - 1) == 0 &&
        same_bits(pooled.value, legacy);
    const bool feature =
        vmafx_feature_score_pooled(s->context, "vmaf", pool, 1, 3, &pooled, NULL) == VMAFX_OK &&
        vmaf_feature_score_pooled(l->vmaf, "vmaf", method, &legacy, 1, 3) == 0 &&
        same_bits(pooled.value, legacy);
    return model && feature && pooled.pool == pool && pooled.first == 1 && pooled.last == 3;
}

static char *test_pooled_matches_libvmaf(void)
{
    Session s;
    Legacy l;
    mu_assert("sessions", open_session(&s, true) && open_legacy(&l));
    for (size_t p = 0; p < sizeof(all_pools) / sizeof(all_pools[0]); p++) {
        mu_assert("every pool method, bit-identical", pooled_case(&s, &l, all_pools[p]));
    }
    mu_assert("close", close_session(&s) && close_legacy(&l));
    return NULL;
}

/* ---- PENDING -------------------------------------------------------------------------------- */

static char *test_pending_is_not_an_error(void)
{
    Session s;
    mu_assert("session", open_session(&s, false));
    /* motion2 of the last frame is final only after the next frame or the
     * flush (design section 2.8). */
    VmafxScore score = VMAFX_SCORE_INIT;
    score.value = -1.0;
    VmafxError *error = NULL;
    const uint64_t last = N_FRAMES - 1;
    mu_assert("pending",
              vmafx_score_frame(s.context, s.model, last, &score, &error) == VMAFX_PENDING);
    mu_assert("no error, nothing written", error == NULL && score.value == -1.0);
    VmafxPooledScore pooled = VMAFX_POOLED_SCORE_INIT;
    mu_assert("pooled pending", vmafx_score_pooled(s.context, s.model, VMAFX_POOL_MEAN, 0, last,
                                                   &pooled, &error) == VMAFX_PENDING &&
                                    error == NULL);
    mu_assert("flush", vmafx_flush(s.context, NULL) == VMAFX_OK);
    mu_assert("final", vmafx_score_frame(s.context, s.model, last, &score, NULL) == VMAFX_OK);
    mu_assert("close", close_session(&s));
    return NULL;
}

/* ---- Refusals ------------------------------------------------------------------------------- */

static char *test_frame_score_refusals(void)
{
    Session s;
    mu_assert("session", open_session(&s, true));
    VmafxScore score = VMAFX_SCORE_INIT;
    VmafxError *error = NULL;
    const uint64_t too_far = (uint64_t)UINT_MAX + 1u;
    mu_assert("range",
              vmafx_score_frame(s.context, s.model, too_far, &score, &error) == VMAFX_E_RANGE &&
                  vt_failed(&error, VMAFX_E_RANGE, "index", VMAFX_SUBJECT_FRAME));
    mu_assert("NULL model",
              vmafx_score_frame(s.context, NULL, 0, &score, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "model", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL set",
              vmafx_score_frame_model_set(s.context, NULL, 0, NULL, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "set", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL feature",
              vmafx_feature_score(s.context, NULL, 0, &score, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "feature", VMAFX_SUBJECT_PARAMETER));
    score.struct_size = 2;
    mu_assert("short out",
              vmafx_score_frame(s.context, s.model, 0, &score, &error) == VMAFX_E_ABI &&
                  vt_failed(&error, VMAFX_E_ABI, "out", VMAFX_SUBJECT_PARAMETER));
    mu_assert("close", close_session(&s));
    return NULL;
}

static char *test_unscored_frame_named(void)
{
    Session s;
    mu_assert("session", open_session(&s, true));
    VmafxScore score = VMAFX_SCORE_INIT;
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_score_frame(s.context, s.model, 99, &score, &error);
    mu_assert("refused", status < 0 && error != NULL);
    mu_assert("names the model and carries the engine's errno",
              !strcmp(vmafx_error_subject(error), "vmaf") &&
                  vmafx_error_subject_kind(error) == VMAFX_SUBJECT_MODEL &&
                  vmafx_error_errno(error) < 0);
    vmafx_error_free(error);
    mu_assert("NULL error pointer", vmafx_score_frame(s.context, s.model, 99, &score, NULL) < 0);
    mu_assert("close", close_session(&s));
    return NULL;
}

static char *test_pooled_refusals(void)
{
    Session s;
    mu_assert("session", open_session(&s, true));
    VmafxPooledScore pooled = VMAFX_POOLED_SCORE_INIT;
    VmafxError *error = NULL;
    mu_assert("pool 0",
              vmafx_score_pooled(s.context, s.model, 0, 0, 1, &pooled, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "pool", VMAFX_SUBJECT_PARAMETER));
    mu_assert("pool 9", vmafx_feature_score_pooled(s.context, "vmaf", 9, 0, 1, &pooled, &error) ==
                                VMAFX_E_INVALID &&
                            vt_failed(&error, VMAFX_E_INVALID, "pool", VMAFX_SUBJECT_PARAMETER));
    mu_assert("first after last",
              vmafx_score_pooled(s.context, s.model, VMAFX_POOL_MEAN, 3, 1, &pooled, &error) ==
                      VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "first", VMAFX_SUBJECT_FRAME));
    mu_assert("close", close_session(&s));
    return NULL;
}

static char *test_pooled_set_refusals(void)
{
    Session s;
    mu_assert("session", open_session(&s, true));
    VmafxPooledScore pooled = VMAFX_POOLED_SCORE_INIT;
    VmafxError *error = NULL;
    const uint64_t too_far = (uint64_t)UINT_MAX + 1u;
    mu_assert("NULL out", vmafx_score_pooled_model_set(s.context, s.set, VMAFX_POOL_MEAN, 0,
                                                       too_far, NULL, &error) == VMAFX_E_INVALID &&
                              vt_failed(&error, VMAFX_E_INVALID, "out", VMAFX_SUBJECT_PARAMETER));
    VmafxModelSetScore set_score = VMAFX_MODEL_SET_SCORE_INIT;
    mu_assert("set range",
              vmafx_score_pooled_model_set(s.context, s.set, VMAFX_POOL_MEAN, 0, too_far,
                                           &set_score, &error) == VMAFX_E_RANGE &&
                  vt_failed(&error, VMAFX_E_RANGE, "last", VMAFX_SUBJECT_FRAME));
    mu_assert("unknown feature", vmafx_feature_score_pooled(s.context, "no_such", VMAFX_POOL_MEAN,
                                                            0, 1, &pooled, &error) < 0 &&
                                     !strcmp(vmafx_error_subject(error), "no_such"));
    vmafx_error_free(error);
    mu_assert("close", close_session(&s));
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_frame_scores_match_libvmaf), MU_TEST(test_set_frame_score),
        MU_TEST(test_set_pooled_score),           MU_TEST(test_pooled_matches_libvmaf),
        MU_TEST(test_pending_is_not_an_error),    MU_TEST(test_frame_score_refusals),
        MU_TEST(test_unscored_frame_named),       MU_TEST(test_pooled_refusals),
        MU_TEST(test_pooled_set_refusals),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
