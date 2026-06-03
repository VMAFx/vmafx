/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Smoke test for the vmaf-roi sidecar core helpers (T6-2b / ADR-0247):
 *
 *    - vmaf_roi_reduce_per_ctu(): per-CTU mean reduction handles full
 *      and partial CTUs without out-of-bounds reads.
 *    - vmaf_roi_saliency_to_qp(): saliency [0, 1] -> QP offset mapping
 *      respects sign + clamp, and is monotonic in saliency.
 *
 *  We do not exercise the I/O paths here; that is covered by the
 *  /-help and end-to-end smoke commands documented in the PR body.
 */

#include <math.h>

#include "test.h"
#include "vmaf_roi_core.h"

static char *test_reduce_full_ctu(void)
{
    /* 4x4 plane, 2x2 CTUs => 2x2 grid. Pre-fill so each CTU has an
     * obvious mean. */
    float sal[16];
    for (int i = 0; i < 16; ++i)
        sal[i] = 0.0F;
    /* Top-left CTU: all 0.25 */
    sal[0] = 0.25F;
    sal[1] = 0.25F;
    sal[4] = 0.25F;
    sal[5] = 0.25F;
    /* Top-right CTU: all 0.75 */
    sal[2] = 0.75F;
    sal[3] = 0.75F;
    sal[6] = 0.75F;
    sal[7] = 0.75F;
    /* Bottom-left CTU: all 1.0 */
    sal[8] = 1.0F;
    sal[9] = 1.0F;
    sal[12] = 1.0F;
    sal[13] = 1.0F;
    /* Bottom-right CTU: all 0.0 (already zero) */

    float grid[4] = {0};
    vmaf_roi_reduce_per_ctu(sal, 4, 4, 2, grid, 2, 2);

    mu_assert("top-left mean wrong", fabsf(grid[0] - 0.25F) < 1e-6F);
    mu_assert("top-right mean wrong", fabsf(grid[1] - 0.75F) < 1e-6F);
    mu_assert("bot-left mean wrong", fabsf(grid[2] - 1.0F) < 1e-6F);
    mu_assert("bot-right mean wrong", fabsf(grid[3] - 0.0F) < 1e-6F);
    return NULL;
}

static char *test_reduce_partial_ctu(void)
{
    /* 5x5 plane with ctu=4 => 2x2 grid; right column + bottom row are
     * partial 1-px-wide / 1-px-tall CTUs. */
    float sal[25];
    for (int i = 0; i < 25; ++i)
        sal[i] = 0.5F;
    float grid[4] = {0};
    vmaf_roi_reduce_per_ctu(sal, 5, 5, 4, grid, 2, 2);
    /* Every cell averages over a uniform plane => 0.5. The partial
     * cells exercise the edge clamping; if it were broken (OOB read /
     * zero-area divide) we'd get NaN or 0. */
    for (int i = 0; i < 4; ++i) {
        mu_assert("partial-CTU mean wrong", fabsf(grid[i] - 0.5F) < 1e-6F);
    }
    return NULL;
}

static char *test_qp_signs(void)
{
    /* High saliency => negative offset; low => positive; mid => 0. */
    mu_assert("sal=1 should give negative", vmaf_roi_saliency_to_qp(1.0F, 6.0) < 0);
    mu_assert("sal=0 should give positive", vmaf_roi_saliency_to_qp(0.0F, 6.0) > 0);
    mu_assert("sal=0.5 should give zero", vmaf_roi_saliency_to_qp(0.5F, 6.0) == 0);
    /* Symmetry. */
    int hi = vmaf_roi_saliency_to_qp(1.0F, 6.0);
    int lo = vmaf_roi_saliency_to_qp(0.0F, 6.0);
    mu_assert("sign symmetry broken", hi == -lo);
    return NULL;
}

static char *test_qp_clamp(void)
{
    /* strength=100 with sal=1 must clamp to -12, not -100. */
    int q_hi = vmaf_roi_saliency_to_qp(1.0F, 100.0);
    int q_lo = vmaf_roi_saliency_to_qp(0.0F, 100.0);
    mu_assert("upper clamp", q_hi == -VMAF_ROI_CORE_QP_OFFSET_MAX);
    mu_assert("lower clamp", q_lo == VMAF_ROI_CORE_QP_OFFSET_MAX);
    return NULL;
}

static char *test_qp_monotonic(void)
{
    /* Walking saliency 0 -> 1 must produce a monotonically non-increasing
     * QP offset (more saliency => more bits => lower offset). */
    int prev = vmaf_roi_saliency_to_qp(0.0F, 6.0);
    for (int i = 1; i <= 20; ++i) {
        const float s = (float)i / 20.0F;
        int cur = vmaf_roi_saliency_to_qp(s, 6.0);
        mu_assert("monotonicity broken", cur <= prev);
        prev = cur;
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_reduce_full_ctu);
    mu_run_test(test_reduce_partial_ctu);
    mu_run_test(test_qp_signs);
    mu_run_test(test_qp_clamp);
    mu_run_test(test_qp_monotonic);
    return NULL;
}
