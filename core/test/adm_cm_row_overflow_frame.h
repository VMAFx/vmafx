/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * A 64x64 luma picture whose scale-0 contrast-masking row passes INT64_MAX
 * (T-ADM-CM-SCALE0-ROW-INT64-OVERFLOW-2026-10-05). Shared by the CPU test
 * (test_integer_adm_cm_row_unsigned.c) and the GPU twin test
 * (test_gpu_adm_tiny_frames.c); its bytes are fixed.
 *
 * Compared with itself the picture has no distortion, so every masking
 * threshold is 0 and each sample of the band's interior adds the full cube
 * of its CSF-weighted coefficient to the row. Pixel rows 31 to 34 drive
 * band row 16: in the columns marked '-' they read (max, max, 0, max), which
 * gives the vertical high-pass its largest value, and in the columns marked
 * '+' they read (0, 0, max, 0), its largest value of the other sign. The
 * column pattern is the one that maximises that row's sum (an exact search
 * over every column sign pattern); every other row is 0. At the default
 * Watson CSF weights the row reaches 1.021 INT64_MAX at 16 bits, 1.018 at 10
 * and 1.009 at 8. A signed row total wraps and the frame fails with a NaN
 * numerator; the unsigned row total holds it.
 */

#ifndef VMAF_TEST_ADM_CM_ROW_OVERFLOW_FRAME_H_
#define VMAF_TEST_ADM_CM_ROW_OVERFLOW_FRAME_H_

#include <stdint.h>

#define ADM_ROW_OVERFLOW_W 64u
#define ADM_ROW_OVERFLOW_H 64u

/* '-': (max, max, 0, max) in rows 31 to 34; '+': (0, 0, max, 0). */
static const char ADM_ROW_OVERFLOW_COLUMNS[ADM_ROW_OVERFLOW_W + 1u] =
    "+++---------------------------------------------------------++++";

/* Luma sample (row, col) of the picture at sample maximum `max`. */
static inline uint16_t adm_row_overflow_sample(unsigned row, unsigned col, uint16_t max)
{
    if (row < 31u || row > 34u || col >= ADM_ROW_OVERFLOW_W) {
        return 0u;
    }
    const int minus = ADM_ROW_OVERFLOW_COLUMNS[col] == '-';
    const int high_row = row != 33u;
    return (minus == high_row) ? max : 0u;
}

#endif /* VMAF_TEST_ADM_CM_ROW_OVERFLOW_FRAME_H_ */
