/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Launch geometry and source rows of the scale-0 vertical DWT of hip ADM
 *  (adm_dwt2_8_vert_hori_kernel_* in adm_dwt2.hip, launched by
 *  dwt2_8_device_hip() / dwt2_16_device_hip() in integer_adm_hip.c).
 *
 *  A block of ADM_DWT2_TILE_COLS x (ADM_DWT2_TILE_ROWS /
 *  ADM_DWT2_V_ROWS_PER_THREAD) threads computes ADM_DWT2_TILE_ROWS vertical
 *  output rows; each thread computes ADM_DWT2_V_ROWS_PER_THREAD of them from
 *  2 + 2 * ADM_DWT2_V_ROWS_PER_THREAD source rows, starting one row above
 *  2 * y_out. The rows are reflected once at each edge, like the CPU's
 *  adm_dwt2(). Threads of the last block row whose outputs lie past the plane
 *  still load: on a plane of 8 rows or fewer one reflection of their rows
 *  lands below zero and the load reads before the picture
 *  (T-CUDA-HIP-ADM-DWT-VERT-TINY-HEIGHT-OOB-2026-09-29). init() refuses
 *  planes below ADM_MIN_FRAME_DIM (17) rows, where every loaded row is
 *  already inside the plane; the clamp keeps the kernel in bounds on its own
 *  and is the identity there, so it cannot change a score (ADR-1381).
 *
 *  Plain C as well as HIP C++ so test_hip_adm_dwt2_rows checks this
 *  arithmetic on the host for every plane height.
 */

#ifndef VMAF_SRC_FEATURE_HIP_INTEGER_ADM_ADM_DWT2_ROWS_H_
#define VMAF_SRC_FEATURE_HIP_INTEGER_ADM_ADM_DWT2_ROWS_H_

#include "../hip_tile_index.h"

/* Vertical outputs per thread, output rows and columns per block. The kernel
 * instantiations in adm_dwt2.hip spell these values in their names and
 * static_assert that they match. */
#define ADM_DWT2_V_ROWS_PER_THREAD 4
#define ADM_DWT2_TILE_ROWS 8
#define ADM_DWT2_TILE_COLS 128

/* Source rows one thread loads. */
#define ADM_DWT2_SOURCE_ROWS (2 + (2 * ADM_DWT2_V_ROWS_PER_THREAD))

/* First vertical output row of thread row `thread_y` in block row `block_y`. */
VMAF_HIP_HOST_DEVICE int adm_dwt2_thread_y_out(int block_y, int thread_y)
{
    return (thread_y + ((block_y * ADM_DWT2_TILE_ROWS) / ADM_DWT2_V_ROWS_PER_THREAD)) *
           ADM_DWT2_V_ROWS_PER_THREAD;
}

/* Source row `i` of the thread whose first output row is `y_out`, reflected
 * once at the bottom (h -> h - 1, h + 1 -> h - 2) and, for the one row above
 * 2 * y_out, at the top (-1 -> 1). Not clamped: see adm_dwt2_source_row(). */
VMAF_HIP_HOST_DEVICE int adm_dwt2_reflect_row(int y_out, int i, int h)
{
    int y_in = (2 * y_out) - 1 + i;
    const int past = (2 * (y_in - h)) + 1;
    y_in -= (past > 0) ? past : 0;
    if (i < 1 && y_in < 0) {
        y_in = -y_in;
    }
    return y_in;
}

/* The row the kernel reads: the reflected row, clamped into the plane for
 * the padding threads whose loads no output consumes. */
VMAF_HIP_HOST_DEVICE int adm_dwt2_source_row(int y_out, int i, int h)
{
    return vmaf_hip_tile_index(adm_dwt2_reflect_row(y_out, i, h), h);
}

#endif /* VMAF_SRC_FEATURE_HIP_INTEGER_ADM_ADM_DWT2_ROWS_H_ */
