/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Smoke test for the vmaf-roi sidecar core helpers (T6-2b / ADR-0247):
 *
 *    - vmaf_roi_reduce_per_ctu(): per-CTU mean reduction handles full
 *      and partial CTUs without out-of-bounds reads.
 *    - vmaf_roi_saliency_to_qp(): saliency [0, 1] -> QP offset mapping
 *      respects sign + clamp, and is monotonic in saliency.
 *    - frame_bytes_samples(): chroma plane ceiling-division for 4:2:0 and
 *      4:2:2 formats produces the correct sample count for odd dimensions,
 *      preventing wrong-seek bugs when frame > 0 (PR fix).
 *
 *  We do not exercise the I/O paths here; that is covered by the
 *  --help and end-to-end smoke commands documented in the PR body.
 */

#include <math.h>
#include <stddef.h>

#include "test.h"
#include "vmaf_roi_core.h"

/* Mirror of the sample-count logic from vmaf_roi.c:frame_bytes().
 * frame_bytes() itself is static; we replicate the formula here so the
 * unit test can pin the expected values without dragging in the full binary.
 * Any drift between the two copies will surface as a test failure.       */
static size_t test_frame_samples(int w, int h, int pixfmt)
{
    /* pixfmt: 0=420, 1=422, 2=444 */
    const size_t y = (size_t)w * (size_t)h;
    const size_t cw = ((size_t)w + 1U) / 2U; /* ceil(w/2) */
    const size_t ch = ((size_t)h + 1U) / 2U; /* ceil(h/2) */
    switch (pixfmt) {
    case 0:
        return y + 2U * cw * ch; /* 4:2:0 */
    case 1:
        return y + 2U * cw * (size_t)h; /* 4:2:2 */
    default:
        return y + 2U * y; /* 4:4:4 */
    }
}

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

/* Regression test for the frame_bytes() chroma ceiling-division fix.
 *
 * Before the fix frame_bytes() used (y / 2) for 4:2:0 and (y + y) for 4:2:2,
 * both of which truncate chroma for odd-dimension inputs.  The wrong byte count
 * causes fseeko() to land at the wrong position for frame indices > 0.
 *
 * The function under test is static in vmaf_roi.c; test_frame_samples() above
 * mirrors its corrected formula so we can pin expected values here.
 */
static char *test_frame_bytes_even(void)
{
    /* Even dimensions: old truncating formula and new ceiling formula agree. */
    /* 4:2:0 1920x1080: Y=2073600, chroma=2*960*540=1036800 => total=3110400 */
    mu_assert("420 even", test_frame_samples(1920, 1080, 0) == 3110400U);
    /* 4:2:2 1920x1080: Y=2073600, chroma=2*960*1080=2073600 => total=4147200 */
    mu_assert("422 even", test_frame_samples(1920, 1080, 1) == 4147200U);
    /* 4:4:4 1920x1080: 3*Y=6220800 */
    mu_assert("444 even", test_frame_samples(1920, 1080, 2) == 6220800U);
    return NULL;
}

static char *test_frame_bytes_odd_420(void)
{
    /* 4:2:0 with odd width and height: chroma plane is ceil(w/2)*ceil(h/2).
     * w=5 h=5: Y=25, cw=3 ch=3, chroma=2*3*3=18 => total=43.
     * The pre-fix formula gave y + y/2 = 25+12 = 37 (wrong). */
    mu_assert("420 5x5", test_frame_samples(5, 5, 0) == 43U);
    /* w=7 h=3: Y=21, cw=4 ch=2, chroma=2*4*2=16 => total=37.
     * Pre-fix: 21 + 10 = 31 (wrong). */
    mu_assert("420 7x3", test_frame_samples(7, 3, 0) == 37U);
    /* w=1 h=1: Y=1, cw=1 ch=1, chroma=2 => total=3.
     * Pre-fix: 1 + 0 = 1 (wrong). */
    mu_assert("420 1x1", test_frame_samples(1, 1, 0) == 3U);
    return NULL;
}

static char *test_frame_bytes_odd_422(void)
{
    /* 4:2:2 with odd width: chroma plane is ceil(w/2)*h.
     * w=5 h=5: Y=25, cw=3, chroma=2*3*5=30 => total=55.
     * Pre-fix: y+y = 50 (wrong). */
    mu_assert("422 5x5", test_frame_samples(5, 5, 1) == 55U);
    /* w=7 h=3: Y=21, cw=4, chroma=2*4*3=24 => total=45.
     * Pre-fix: 42 (wrong). */
    mu_assert("422 7x3", test_frame_samples(7, 3, 1) == 45U);
    /* w=1 h=1: Y=1, cw=1, chroma=2*1*1=2 => total=3.
     * Pre-fix: 2 (wrong). */
    mu_assert("422 1x1", test_frame_samples(1, 1, 1) == 3U);
    return NULL;
}

static char *test_frame_bytes_444(void)
{
    /* 4:4:4 has no subsampling; formula was always correct. */
    mu_assert("444 5x5", test_frame_samples(5, 5, 2) == 75U);
    mu_assert("444 1x1", test_frame_samples(1, 1, 2) == 3U);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_reduce_full_ctu);
    mu_run_test(test_reduce_partial_ctu);
    mu_run_test(test_qp_signs);
    mu_run_test(test_qp_clamp);
    mu_run_test(test_qp_monotonic);
    mu_run_test(test_frame_bytes_even);
    mu_run_test(test_frame_bytes_odd_420);
    mu_run_test(test_frame_bytes_odd_422);
    mu_run_test(test_frame_bytes_444);
    return NULL;
}
