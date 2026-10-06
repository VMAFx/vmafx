/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The cells of the VMAFx import bit-exactness tests (RC4 WP3, ADR-2023,
 * ADR-2091): a twin declared exact (scripts/ci/exact_twins.d/<cell>.<backend>,
 * ADR-1428) as the CPU extractor and options a VMAFx context on a device
 * registers for it (the aliases of scripts/ci/cross_backend_parity_gate.py
 * FEATURE_ALIASES), and the helpers the backend lanes' tests share: one
 * context per cell on the device, and the comparison of two contexts' scores
 * bit for bit. Each table is its own header (vmafx_device_cells.h for the
 * CUDA and HIP lanes, vmafx_sycl_cells.h), held to the fragments by a
 * contract test.
 */

#ifndef VMAFX_EXACT_CELLS_H
#define VMAFX_EXACT_CELLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "feature/feature_collector.h"
#include "libvmaf_priv.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/vmafx.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C header. The fork builds C as C23,
 * where clang-tidy also proposes the `nullptr` keyword, but MSVC's documented
 * /std:clatest C23 feature set does not include `nullptr`. ADR-1138. */

typedef struct VcCell {
    const char *name;      /* the fragment's cell */
    const char *extractor; /* the CPU extractor the context registers */
    const char *options;   /* "key=value:key=value", or NULL */
    unsigned min_chroma;   /* smallest chroma plane side the cell scores */
} VcCell;

/* VMAFX_TEST_IMPORT_ONLY=1: import sessions only, nothing compared, for a
 * profiler trace that shows the import's copies alone (the host sessions
 * upload every frame by design). */
static inline bool vc_import_only(void)
{
    /* NOLINTNEXTLINE(concurrency-mt-unsafe): single-thread test setup (ADR-0141 / ADR-0278). */
    const char *const v = getenv("VMAFX_TEST_IMPORT_ONLY");
    return v && v[0] == '1';
}

/* The options of `spec` ("k=v:k=v"); NULL for none or on a failure. */
static inline VmafxOptions *vc_options(const char *spec)
{
    char buf[160];
    VmafxOptions *options = NULL;
    if (!spec || strlen(spec) >= sizeof(buf)) {
        return NULL;
    }
    memcpy(buf, spec, strlen(spec) + 1u);
    char *item = buf;
    for (unsigned guard = 0; item && guard < 8u; guard++) {
        char *const next = strchr(item, ':');
        if (next) {
            *next = '\0';
        }
        char *const eq = strchr(item, '=');
        if (!eq) {
            break;
        }
        *eq = '\0';
        (void)vmafx_options_set(&options, item, eq + 1, NULL);
        item = next ? next + 1 : NULL;
    }
    return options;
}

/* A context on `device` that registers `cell`, or NULL. */
static inline VmafxContext *vc_cell_context(VmafxDevice *device, const VcCell *cell)
{
    VmafxContext *context = NULL;
    if (vmafx_context_create(NULL, &context, NULL) != VMAFX_OK) {
        return NULL;
    }
    VmafxOptions *const options = vc_options(cell->options);
    const bool ok = vmafx_context_use_device(context, device, NULL) == VMAFX_OK &&
                    vmafx_context_use_feature(context, cell->extractor, options, NULL) == VMAFX_OK;
    vmafx_options_free(options);
    if (!ok) {
        (void)vmafx_context_destroy(context, NULL);
        return NULL;
    }
    return context;
}

/* Every feature `a` collected at frames 0 .. n-1 equals `b`'s bit for bit;
 * the values compared are added to `*compared`, the differing ones to
 * `*differing`. */
static inline bool vc_compare(VmafxContext *a, VmafxContext *b, unsigned n, unsigned long *compared,
                              unsigned long *differing)
{
    const VmafFeatureCollector *fa = vmaf_feature_collector_get(vmafx_context_libvmaf_handle(a));
    const VmafFeatureCollector *fb = vmaf_feature_collector_get(vmafx_context_libvmaf_handle(b));
    if (!fa || !fb || fa->cnt != fb->cnt || fa->cnt == 0u) {
        return false;
    }
    for (unsigned f = 0; f < fa->cnt; f++) {
        const char *name = fa->feature_vector[f]->name;
        for (unsigned i = 0; i < n; i++) {
            VmafxScore sa = VMAFX_SCORE_INIT;
            VmafxScore sb = VMAFX_SCORE_INIT;
            const VmafxStatus ra = vmafx_feature_score(a, name, i, &sa, NULL);
            const VmafxStatus rb = vmafx_feature_score(b, name, i, &sb, NULL);
            *compared += 1u;
            const bool differs = ra != rb || (ra == VMAFX_OK && !vt_same_bits(sa.value, sb.value));
            *differing += differs;
            /* NOLINTNEXTLINE(concurrency-mt-unsafe): single-thread test reporting (ADR-0141 / ADR-0278). */
            if (differs && getenv("VMAFX_TEST_VERBOSE")) {
                (void)fprintf(stderr, "\n    %s[%u]: %.17g (%d) vs %.17g (%d)", name, i, sa.value,
                              (int)ra, sb.value, (int)rb);
            }
        }
    }
    return true;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAFX_EXACT_CELLS_H */
