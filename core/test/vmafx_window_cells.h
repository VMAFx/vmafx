/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Device windows on exact twins (RC4 WP4, #2471): windows opened on the
 * imported context before frames are submitted must complete and equal the
 * synchronous pooled values of both the device and host contexts bit for bit.
 */

#ifndef VMAFX_WINDOW_CELLS_H
#define VMAFX_WINDOW_CELLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "feature/feature_collector.h"
#include "libvmaf_priv.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"
#include "vmafx_test_util.h"
#include "vmafx_window_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C header. The fork builds C as C23,
 * where clang-tidy also proposes the `nullptr` keyword, but MSVC's documented
 * /std:clatest C23 feature set does not include `nullptr`. ADR-1138. */

enum {
    VWC_MAX_RANGES = 4,
    VWC_MAX_FEATURES = 64,
    VWC_MAX_WINDOWS = VWC_MAX_RANGES * VWC_MAX_FEATURES,
};

typedef struct VwcWindow {
    VmafxWindow *window;
    const char *feature;
    uint64_t first;
    uint64_t last;
} VwcWindow;

typedef struct VwcSet {
    VwcWindow windows[VWC_MAX_WINDOWS];
    unsigned count;
} VwcSet;

typedef struct VwcRange {
    uint64_t first;
    uint64_t last;
} VwcRange;

/* Release every window in `set` and reset its count to 0. */
static inline void vwc_release(VwcSet *set)
{
    if (!set) {
        return;
    }
    for (unsigned i = 0; i < set->count && i < VWC_MAX_WINDOWS; i++) {
        if (set->windows[i].window) {
            vmafx_window_release(set->windows[i].window);
            set->windows[i].window = NULL;
        }
    }
    set->count = 0u;
}

/* Collect unique ranges [0,n-1], [0,0], [n/3, 2n/3], [n-1,n-1] for n frames. */
static inline unsigned vwc_dedupe_ranges(unsigned n, VwcRange out[VWC_MAX_RANGES])
{
    if (n == 0u) {
        return 0u;
    }
    const VwcRange candidates[VWC_MAX_RANGES] = {
        {0u, (uint64_t)(n - 1u)},
        {0u, 0u},
        {(uint64_t)(n / 3u), (uint64_t)(2u * (uint64_t)n / 3u)},
        {(uint64_t)(n - 1u), (uint64_t)(n - 1u)},
    };
    unsigned count = 0u;
    for (unsigned i = 0; i < VWC_MAX_RANGES; i++) {
        bool dup = false;
        for (unsigned k = 0; k < count && k < VWC_MAX_RANGES; k++) {
            if (out[k].first == candidates[i].first && out[k].last == candidates[i].last) {
                dup = true;
                break;
            }
        }
        if (!dup && count < VWC_MAX_RANGES) {
            out[count++] = candidates[i];
        }
    }
    return count;
}

static inline const VmafFeatureCollector *vwc_collector(VmafxContext *host)
{
    const VmafFeatureCollector *const fc =
        vmaf_feature_collector_get(vmafx_context_libvmaf_handle(host));
    if (!fc || fc->cnt == 0u || fc->cnt > VWC_MAX_FEATURES) {
        return NULL;
    }
    return fc;
}

/* Submit windows on `imp` for ranges [0,n-1], [0,0], [n/3, 2n/3], [n-1,n-1]
 * for every feature name in host's collector. */
static inline bool vwc_open(VmafxContext *imp, VmafxContext *host, unsigned n, VwcSet *set)
{
    if (!imp || !host || !set || n == 0u) {
        return false;
    }
    set->count = 0u;
    const VmafFeatureCollector *const fc = vwc_collector(host);
    if (!fc) {
        return false;
    }
    VwcRange ranges[VWC_MAX_RANGES];
    const unsigned n_ranges = vwc_dedupe_ranges(n, ranges);
    for (unsigned f = 0; f < fc->cnt && f < VWC_MAX_FEATURES; f++) {
        const char *const name = fc->feature_vector[f]->name;
        for (unsigned r = 0; r < n_ranges && r < VWC_MAX_RANGES; r++) {
            if (set->count >= VWC_MAX_WINDOWS) {
                vwc_release(set);
                return false;
            }
            VmafxWindow *const w =
                vw_submit(imp, vw_feature(name), ranges[r].first, ranges[r].last);
            if (!w) {
                vwc_release(set);
                return false;
            }
            set->windows[set->count].window = w;
            set->windows[set->count].feature = name;
            set->windows[set->count].first = ranges[r].first;
            set->windows[set->count].last = ranges[r].last;
            set->count++;
        }
    }
    return true;
}

static inline bool vwc_check_window(VmafxContext *imp, VmafxContext *host, const VwcWindow *w,
                                    unsigned long *compared, unsigned long *differing)
{
    VmafxWindowResult r;
    const bool complete = vw_complete(w->window, &r);
    if (!complete) {
        *differing += 1u;
        return false;
    }
    const bool same_sync = vw_same_as_sync(imp, vw_feature(w->feature), &r);
    for (uint32_t pool = VMAFX_POOL_MIN; pool <= VMAFX_POOL_PERC20; pool++) {
        if ((VW_ALL_POOLS & (1u << pool)) == 0u) {
            continue;
        }
        VmafxPooledScore hs = VMAFX_POOLED_SCORE_INIT;
        const VmafxStatus hstat =
            vmafx_feature_score_pooled(host, w->feature, pool, w->first, w->last, &hs, NULL);
        *compared += 1u;
        const bool match =
            same_sync && (hstat == VMAFX_OK) && vt_same_bits(r.value[pool], hs.value);
        if (!match) {
            *differing += 1u;
        }
    }
    return true;
}

/* Check every window in `set`: completed, matches synchronous pooled on `imp`,
 * and matches host pooled scores bit for bit across VW_ALL_POOLS. */
static inline bool vwc_check(VmafxContext *imp, VmafxContext *host, VwcSet *set,
                             unsigned long *compared, unsigned long *differing)
{
    if (!imp || !host || !set || !compared || !differing || set->count == 0u) {
        return false;
    }
    bool complete_ok = true;
    for (unsigned i = 0; i < set->count && i < VWC_MAX_WINDOWS; i++) {
        if (!vwc_check_window(imp, host, &set->windows[i], compared, differing)) {
            complete_ok = false;
        }
    }
    return complete_ok;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAFX_WINDOW_CELLS_H */
