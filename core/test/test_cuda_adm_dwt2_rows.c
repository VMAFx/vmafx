/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Device-free check of the rows and taps the cuda integer ADM DWT kernels
 *  load (T-CUDA-HIP-ADM-DWT-VERT-TINY-HEIGHT-OOB-2026-09-29, ADR-1374).
 *
 *  The kernels and their host launch take their geometry and index arithmetic
 *  from core/src/feature/cuda/integer_adm/adm_dwt2_rows.h, and so does this
 *  test, which replays every thread row of the launched grid for every plane
 *  height up to 8192:
 *
 *  - scale 0, from the ADM minimum (ADM_MIN_FRAME_DIM, 17 rows) up: the single
 *    reflection already keeps every loaded row inside the plane, so the clamp
 *    is the identity and cannot change a score;
 *  - scale 0, at every height: the clamped row is inside the plane, so the
 *    kernel never reads outside the picture, whatever init() admits;
 *  - scale 0, below 9 rows: the unclamped reflection does leave the plane (the
 *    defect the SYCL twin hit), which shows the first check can fail;
 *  - scales 1-3: every tap of every launched output is inside its plane for
 *    the planes the ladder produces from 17 rows up, and a 1-row plane would
 *    escape, which shows that check can fail too.
 */

#include <stdio.h>

#include "test.h"

#include "feature/adm_csf_fixed_point.h"
#include "feature/cuda/integer_adm/adm_dwt2_rows.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#define MAX_HEIGHT 8192
#define NUM_SCALES 4

/* Block rows of the scale-0 launch: DIV_ROUND_UP((h + 1) / 2,
 * ADM_DWT2_TILE_ROWS), as dwt2_8_device() and adm_dwt2_16_device() compute
 * it. */
static int grid_rows(int h)
{
    const int out_rows = (h + 1) / 2;
    return (out_rows + ADM_DWT2_TILE_ROWS - 1) / ADM_DWT2_TILE_ROWS;
}

/* Number of scale-0 loads of the launched grid for a plane of `h` rows whose
 * row leaves the plane; `clamped` selects the kernel's read or the bare
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

/* Number of scale 1-3 taps outside a plane of `upper` samples over the
 * launched outputs 0 .. (upper + 1) / 2 - 1 (adm_dwt2_s123_combined_device()
 * launches one block row, or one thread, per output). */
static unsigned escaping_taps(int upper)
{
    unsigned escapes = 0;
    for (int n = 0; n < (upper + 1) / 2; n++) {
        for (int k = 0; k < 4; k++) {
            const int tap = adm_dwt2_s123_tap(n, k, upper);
            escapes += (tap < 0 || tap >= upper) ? 1u : 0u;
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
    for (int h = 1; h <= MAX_HEIGHT; h++) {
        const int last_thread_row = (ADM_DWT2_TILE_ROWS / ADM_DWT2_V_ROWS_PER_THREAD) - 1;
        const int last = adm_dwt2_thread_y_out(grid_rows(h) - 1, last_thread_row) +
                         ADM_DWT2_V_ROWS_PER_THREAD - 1;
        mu_assert("the launch grid must cover every output row", last >= ((h + 1) / 2) - 1);
    }
    return NULL;
}

static char *test_s123_taps_stay_inside_the_ladder(void)
{
    /* integer_adm_cuda.c halves each dimension with (dim + 1) / 2 before
     * every scale, so scales 1-3 run on these planes. */
    for (int h = (int)ADM_MIN_FRAME_DIM; h <= MAX_HEIGHT; h++) {
        int upper = h;
        for (int scale = 1; scale < NUM_SCALES; scale++) {
            upper = (upper + 1) / 2;
            if (escaping_taps(upper) != 0u) {
                (void)fprintf(stderr, "\n  h=%d scale %d (%d samples): a tap escapes\n", h, scale,
                              upper);
                return "a scale 1-3 tap must stay inside its plane";
            }
        }
    }
    mu_assert("a 1-sample plane must escape the single reflection", escaping_taps(1) != 0u);
    mu_assert("a 2-sample plane must stay inside", escaping_taps(2) == 0u);
    return NULL;
}

static char *test_s123_taps_follow_the_dwt_pattern(void)
{
    /* Output 0 reads {1, 0, 1, 2}; output n > 0 reads 2n - 1 .. 2n + 2,
     * reflected once past the end (upper -> upper - 1). */
    static const int first[4] = {1, 0, 1, 2};
    for (int k = 0; k < 4; k++) {
        mu_assert("output 0 must read taps 1, 0, 1, 2", adm_dwt2_s123_tap(0, k, 64) == first[k]);
        mu_assert("an interior output must read 2n - 1 + k",
                  adm_dwt2_s123_tap(10, k, 64) == 19 + k);
    }
    mu_assert("the tap past the end must reflect onto the last sample",
              adm_dwt2_s123_tap(4, 2, 9) == 8 && adm_dwt2_s123_tap(4, 3, 9) == 7);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_reflection_stays_inside_from_the_adm_minimum);
    mu_run_test(test_kernel_reads_stay_inside_at_every_height);
    mu_run_test(test_tiny_planes_would_escape_without_the_clamp);
    mu_run_test(test_thread_rows_cover_every_output_row);
    mu_run_test(test_s123_taps_stay_inside_the_ladder);
    mu_run_test(test_s123_taps_follow_the_dwt_pattern);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
