/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Device-free check of the rows the hip ADM scale-0 vertical DWT loads
 *  (T-CUDA-HIP-ADM-DWT-VERT-TINY-HEIGHT-OOB-2026-09-29, ADR-1381).
 *
 *  The kernel and its host launch take their geometry and row arithmetic from
 *  core/src/feature/hip/integer_adm/adm_dwt2_rows.h, and so does this test,
 *  which replays every thread row of the launched grid for every plane height
 *  up to 8192:
 *
 *  - from the ADM minimum (ADM_MIN_FRAME_DIM, 17 rows) up, the single
 *    reflection already keeps every loaded row inside the plane, so the clamp
 *    is the identity and cannot change a score;
 *  - at every height the clamped row is inside the plane, so the kernel never
 *    reads outside the picture, whatever init() admits;
 *  - below 9 rows the unclamped reflection does leave the plane (the defect
 *    the SYCL twin hit), which shows the first check can fail.
 *
 *  The motion kernels' tile index (hip_tile_index.h) gets the same replay:
 *  every tile a 16x16 block of motion_v2_score.hip or float_motion_score.hip
 *  (both 16x16 blocks, 5-tap filter, the same clamped reflect-101 index)
 *  loads stays inside planes from 3x3 up, and the clamp is the identity for
 *  every sample an output consumes.
 */

#include <stdio.h>

#include "test.h"

#include "feature/adm_csf_fixed_point.h"
#include "feature/hip/hip_tile_index.h"
#include "feature/hip/integer_adm/adm_dwt2_rows.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#define MAX_HEIGHT 8192

/* motion_v2_score.hip and float_motion_score.hip: 16x16 blocks, 5-tap filter
 * (radius 2). */
#define MOTION_BLOCK 16
#define MOTION_RADIUS 2
#define MOTION_MAX_DIM 1024

/* Block rows of the launch: DIV_ROUND_UP((h + 1) / 2, ADM_DWT2_TILE_ROWS), as
 * dwt2_8_device_hip() and dwt2_16_device_hip() compute it. */
static int grid_rows(int h)
{
    const int out_rows = (h + 1) / 2;
    return (out_rows + ADM_DWT2_TILE_ROWS - 1) / ADM_DWT2_TILE_ROWS;
}

/* Number of loads of the launched grid for a plane of `h` rows whose row
 * leaves the plane; `clamped` selects the kernel's read or the bare
 * reflection. */
static unsigned escaping_loads(int h, int clamped)
{
    const int thread_rows = ADM_DWT2_TILE_ROWS / ADM_DWT2_V_ROWS_PER_THREAD;
    unsigned escapes = 0;
    for (int by = 0; by < grid_rows(h); by++) {
        for (int ty = 0; ty < thread_rows; ty++) {
            const int y_out = adm_dwt2_thread_y_out(by, ty);
            for (int i = 0; i < ADM_DWT2_SOURCE_ROWS; i++) {
                const int row =
                    clamped ? adm_dwt2_source_row(y_out, i, h) : adm_dwt2_reflect_row(y_out, i, h);
                escapes += (row < 0 || row >= h) ? 1u : 0u;
            }
        }
    }
    return escapes;
}

static char *test_reflection_stays_inside_from_the_adm_minimum(void)
{
    for (int h = (int)ADM_MIN_FRAME_DIM; h <= MAX_HEIGHT; h++) {
        if (escaping_loads(h, 0) != 0u) {
            (void)fprintf(stderr, "\n  h=%d: a reflected row leaves the plane\n", h);
            return "above the ADM minimum the clamp must be the identity";
        }
    }
    return NULL;
}

static char *test_kernel_reads_stay_inside_at_every_height(void)
{
    for (int h = 1; h <= MAX_HEIGHT; h++) {
        if (escaping_loads(h, 1) != 0u) {
            (void)fprintf(stderr, "\n  h=%d: a clamped row leaves the plane\n", h);
            return "the clamped kernel read must stay inside the plane";
        }
    }
    return NULL;
}

static char *test_tiny_planes_would_escape_without_the_clamp(void)
{
    unsigned heights_escaping = 0;
    for (int h = 1; h < (int)ADM_MIN_FRAME_DIM; h++) {
        heights_escaping += (escaping_loads(h, 0) != 0u) ? 1u : 0u;
    }
    mu_assert("an 8-row plane must escape the bare reflection", escaping_loads(8, 0) != 0u);
    mu_assert("a 9-row plane must stay inside the bare reflection", escaping_loads(9, 0) == 0u);
    mu_assert("exactly the heights 1 to 8 escape the bare reflection", heights_escaping == 8u);
    return NULL;
}

static char *test_thread_rows_cover_every_output_row(void)
{
    /* The last thread row of the grid reaches the last output row. */
    for (int h = 1; h <= 4096; h++) {
        const int last =
            adm_dwt2_thread_y_out(grid_rows(h) - 1,
                                  (ADM_DWT2_TILE_ROWS / ADM_DWT2_V_ROWS_PER_THREAD) - 1) +
            ADM_DWT2_V_ROWS_PER_THREAD - 1;
        mu_assert("the launch grid must cover every output row", last >= ((h + 1) / 2) - 1);
    }
    return NULL;
}

/* One axis of a motion tile load: the index the kernel reads for tile slot
 * `t` of block `b` on a plane of `n` samples. */
static int motion_tile_index(int b, int t, int n)
{
    const int origin = (b * MOTION_BLOCK) - MOTION_RADIUS;
    return vmaf_hip_tile_index(vmaf_hip_reflect_101(origin + t, n), n);
}

/* Every tile slot the grid loads stays in [0, n); the slots an output
 * consumes (within MOTION_RADIUS of an in-plane output) equal the bare
 * reflect-101 index, i.e. the CPU's mirror(). */
static char *check_motion_slot(int b, int t, int n)
{
    const int idx = motion_tile_index(b, t, n);
    mu_assert("a motion tile load leaves the plane", idx >= 0 && idx < n);
    const int pos = (b * MOTION_BLOCK) - MOTION_RADIUS + t;
    const int consumed = pos >= -MOTION_RADIUS && pos < n + MOTION_RADIUS;
    mu_assert("the clamp must not change a consumed motion tap",
              !consumed || idx == vmaf_hip_reflect_101(pos, n));
    return NULL;
}

static char *check_motion_axis(int n)
{
    const int blocks = (n + MOTION_BLOCK - 1) / MOTION_BLOCK;
    const int tile = MOTION_BLOCK + (2 * MOTION_RADIUS);
    for (int b = 0; b < blocks; b++) {
        for (int t = 0; t < tile; t++) {
            mu_assert_msg(check_motion_slot(b, t, n));
        }
    }
    return NULL;
}

static char *test_motion_tile_loads_stay_inside(void)
{
    for (int n = 3; n <= MOTION_MAX_DIM; n++) {
        char *msg = check_motion_axis(n);
        if (msg) {
            (void)fprintf(stderr, "\n  motion plane extent %d\n", n);
            return msg;
        }
    }
    /* The defect the clamp removes: at 17 the last block's halo reflects 33
     * to -1, and on a 3-sample plane the tile's last slot (17) reflects to
     * -13 (float_motion's fm_mirror() read both before the plane). */
    mu_assert("reflect-101 of 33 on a 17-sample plane must be -1",
              vmaf_hip_reflect_101(33, 17) == -1);
    mu_assert("reflect-101 of 17 on a 3-sample plane must be -13",
              vmaf_hip_reflect_101(17, 3) == -13);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_reflection_stays_inside_from_the_adm_minimum);
    mu_run_test(test_kernel_reads_stay_inside_at_every_height);
    mu_run_test(test_tiny_planes_would_escape_without_the_clamp);
    mu_run_test(test_thread_rows_cover_every_output_row);
    mu_run_test(test_motion_tile_loads_stay_inside);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
