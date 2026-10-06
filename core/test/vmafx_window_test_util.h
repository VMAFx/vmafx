/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Helpers shared by the VMAFx window tests (RC4 WP4, ADR-2074): submitting a
 * window, and holding its result against the synchronous pooled scores of a
 * context, method by method, bit for bit.
 */

#ifndef VMAFX_WINDOW_TEST_UTIL_H
#define VMAFX_WINDOW_TEST_UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "vmafx/vmafx.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C header. The fork builds C as C23,
 * where clang-tidy also proposes the `nullptr` keyword, but MSVC's documented
 * /std:clatest C23 feature set does not include `nullptr` and the required
 * Windows builds compile the tests with cl.exe (C2065). ADR-1138. */

/* Every pooling method. */
#define VW_ALL_POOLS                                                                               \
    (VMAFX_POOL_MASK_MIN | VMAFX_POOL_MASK_MAX | VMAFX_POOL_MASK_MEAN |                            \
     VMAFX_POOL_MASK_HARMONIC_MEAN | VMAFX_POOL_MASK_MEDIAN | VMAFX_POOL_MASK_PERC5 |              \
     VMAFX_POOL_MASK_PERC10 | VMAFX_POOL_MASK_PERC20)

/* What a window pools, as the request names it. */
typedef struct VwTarget {
    uint32_t kind;
    const VmafxModel *model;
    const VmafxModelSet *set;
    const char *feature;
} VwTarget;

static inline VwTarget vw_feature(const char *feature)
{
    VwTarget t = {VMAFX_WINDOW_TARGET_FEATURE, NULL, NULL, feature};
    return t;
}

static inline VwTarget vw_model(const VmafxModel *model)
{
    VwTarget t = {VMAFX_WINDOW_TARGET_MODEL, model, NULL, NULL};
    return t;
}

static inline VwTarget vw_set(const VmafxModelSet *set)
{
    VwTarget t = {VMAFX_WINDOW_TARGET_MODEL_SET, NULL, set, NULL};
    return t;
}

static inline VmafxWindowRequest vw_request(VwTarget t, uint64_t first, uint64_t last,
                                            uint32_t mask)
{
    VmafxWindowRequest r = VMAFX_WINDOW_REQUEST_INIT;
    r.target = t.kind;
    r.model = t.model;
    r.model_set = t.set;
    r.feature = t.feature;
    r.first = first;
    r.last = last;
    r.pool_mask = mask;
    return r;
}

/* A window of `t` over [first, last] with every method; NULL on failure. */
static inline VmafxWindow *vw_submit(VmafxContext *context, VwTarget t, uint64_t first,
                                     uint64_t last)
{
    const VmafxWindowRequest r = vw_request(t, first, last, VW_ALL_POOLS);
    VmafxWindow *window = NULL;
    return vmafx_window_submit(context, &r, &window, NULL) == VMAFX_OK ? window : NULL;
}

/* True when the window is complete (poll answers VMAFX_OK). */
static inline bool vw_done(const VmafxWindow *window, VmafxWindowResult *out)
{
    *out = (VmafxWindowResult)VMAFX_WINDOW_RESULT_INIT;
    return vmafx_window_poll(window, out, NULL) == VMAFX_OK;
}

/* The synchronous call's value of `pool` over [first, last] matches slot
 * `pool` of `r` bit for bit. */
static inline bool vw_same_pool(VmafxContext *context, VwTarget t, const VmafxWindowResult *r,
                                uint32_t pool, uint64_t last)
{
    if (t.kind == VMAFX_WINDOW_TARGET_MODEL_SET) {
        VmafxModelSetScore s = VMAFX_MODEL_SET_SCORE_INIT;
        return vmafx_score_pooled_model_set(context, t.set, pool, r->first, last, &s, NULL) ==
                   VMAFX_OK &&
               vt_same_bits(s.bagging, r->value[pool]) && vt_same_bits(s.stddev, r->stddev[pool]) &&
               vt_same_bits(s.ci95_lo, r->ci95_lo[pool]) &&
               vt_same_bits(s.ci95_hi, r->ci95_hi[pool]);
    }
    VmafxPooledScore s = VMAFX_POOLED_SCORE_INIT;
    const VmafxStatus status =
        t.kind == VMAFX_WINDOW_TARGET_MODEL ?
            vmafx_score_pooled(context, t.model, pool, r->first, last, &s, NULL) :
            vmafx_feature_score_pooled(context, t.feature, pool, r->first, last, &s, NULL);
    return status == VMAFX_OK && vt_same_bits(s.value, r->value[pool]);
}

/* Every requested method of `r` equals the synchronous call on `context`
 * over the frames the window pooled. */
static inline bool vw_same_as_sync(VmafxContext *context, VwTarget t, const VmafxWindowResult *r)
{
    if (r->status != VMAFX_OK || r->n_frames == 0) {
        return false;
    }
    const uint64_t last = r->first + r->n_frames - 1u;
    for (uint32_t pool = VMAFX_POOL_MIN; pool <= VMAFX_POOL_PERC20; pool++) {
        if ((r->pool_mask & (1u << pool)) && !vw_same_pool(context, t, r, pool, last)) {
            return false;
        }
    }
    return true;
}

/* Submit frames [from, to] of deterministic content (vt_fill) to `context`. */
static inline bool vw_submit_frames(VmafxContext *context, const VmafxFrameDesc *desc,
                                    unsigned from, unsigned to)
{
    uint8_t *data = malloc(vt_frame_bytes(desc));
    bool ok = data != NULL;
    for (unsigned i = from; i <= to && ok; i++) {
        vt_fill(desc, data, i);
        VmafxFrame *ref = vt_copy_frame(desc, data);
        vt_fill(desc, data, i * 7u + 50u);
        VmafxFrame *dist = vt_copy_frame(desc, data);
        /* Consumes both on every path, a NULL one included. */
        ok = vmafx_submit(context, ref, dist, i, NULL) == VMAFX_OK;
    }
    free(data);
    return ok;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAFX_WINDOW_TEST_UTIL_H */
