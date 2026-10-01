/**
 *
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  The decimation float_ssim_hip runs before its SSIM passes when the
 *  resolved scale is above 1 (ADR-1405): one output sample of
 *  iqa/decimate.c::iqa_decimate() with the low-pass kernel of
 *  ssim.c::ssim_low_pass_alloc(), computed from the raw picture plane.
 *
 *  The CPU forms each output as iqa_filter_pixel() at (x * scale, y * scale):
 *  for every tap of the scale x scale window, `prod = sample * tap` in fp32
 *  with `tap = 1.0f / (scale * scale)`, summed in double, and the sum rounded
 *  to fp32. Samples are picture_copy() values, multiples of 2^-8 below 256.
 *  For scale <= 128 the tap is at least 2^-14, so every non-zero product is a
 *  multiple of 2^-45 and a window sums below 2^8: the double sum is exact, and
 *  so is an int64 sum in units of 2^-52 (below 2^60). One round-to-nearest
 *  conversion of that integer to fp32 followed by an exact scaling by 2^-52
 *  is therefore the CPU's `(float)sum`, bit for bit, with no fp64
 *  (Research-2130 has the proof; the SYCL twin uses the same arithmetic,
 *  ADR-1370).
 *
 *  Plain C as well as HIP C++: the kernel in ssim_score.hip and the
 *  device-free test test_hip_float_ssim_decimate.c compile the same lines,
 *  and the test holds them against iqa_decimate() itself.
 */

#ifndef VMAF_SRC_FEATURE_HIP_FLOAT_SSIM_SSIM_DECIMATE_H_
#define VMAF_SRC_FEATURE_HIP_FLOAT_SSIM_SSIM_DECIMATE_H_

#include <stddef.h>
#include <stdint.h>

#include "../hip_tile_index.h"

/* Largest scale whose window sum is exact in int64 units of 2^-52. */
#define VMAF_HIP_SSIM_MAX_EXACT_SCALE 128

/* 2^52 and 2^-52: the fixed-point unit of the window sum. */
#define VMAF_HIP_SSIM_FIXED_ONE 4503599627370496.0f
#define VMAF_HIP_SSIM_FIXED_INV (1.0f / 4503599627370496.0f)

/* One plane to decimate: packed rows of `sample_bytes`-byte samples, and the
 * two fp32 factors of a tap product. `sample_scale` is picture_copy()'s
 * divisor as a reciprocal (1, 1/4, 1/16 or 1/256, all exact); `tap_weight`
 * is ssim.c's `1.0f / (float)(scale * scale)`, computed on the host. */
typedef struct VmafHipSsimDecimate {
    const uint8_t *plane;
    unsigned sample_bytes;
    unsigned width;
    unsigned height;
    int scale;
    float sample_scale;
    float tap_weight;
} VmafHipSsimDecimate;

/* iqa/convolve.c::KBND_SYMMETRIC: period-2n mirror, edge sample repeated.
 * Identity inside the plane, so it also covers iqa_filter_pixel()'s
 * direct-read interior path. */
VMAF_HIP_HOST_DEVICE int vmaf_hip_ssim_symmetric_index(int position, int extent)
{
    const int period = 2 * extent;
    int folded = position % period;
    if (folded < 0) {
        folded += period;
    }
    return (folded >= extent) ? period - folded - 1 : folded;
}

/* The picture_copy() value of sample `x` of a packed row. Two-byte samples
 * are little-endian, as VmafPicture stores them. */
VMAF_HIP_HOST_DEVICE float vmaf_hip_ssim_sample(const VmafHipSsimDecimate *d, const uint8_t *row,
                                                int x)
{
    if (d->sample_bytes == 2u) {
        const size_t low = (size_t)x * 2u;
        const unsigned raw = (unsigned)row[low] | ((unsigned)row[low + 1u] << 8u);
        return (float)raw * d->sample_scale;
    }
    return (float)row[x] * d->sample_scale;
}

/* fp32 product -> integer units of 2^-52. Exact: the scaling is by a power
 * of two and the result is an integer below 2^60. */
VMAF_HIP_HOST_DEVICE int64_t vmaf_hip_ssim_fixed(float product)
{
    return (int64_t)(product * VMAF_HIP_SSIM_FIXED_ONE);
}

/* One output of iqa_decimate(): the window centred on (centre_x, centre_y) =
 * (x * scale, y * scale). Row r of the window is offset r - scale / 2,
 * iqa_filter_pixel()'s -vc .. vc - kh_even for odd and even scales alike.
 * The loops are bounded by VMAF_HIP_SSIM_MAX_EXACT_SCALE, which the caller
 * enforces. */
VMAF_HIP_HOST_DEVICE float vmaf_hip_ssim_decimate_sample(const VmafHipSsimDecimate *d, int centre_x,
                                                         int centre_y)
{
    const int half = d->scale / 2;
    const size_t row_bytes = (size_t)d->width * d->sample_bytes;
    int64_t sum = 0;
    for (int r = 0; r < d->scale; r++) {
        const int source_y = vmaf_hip_ssim_symmetric_index(centre_y + r - half, (int)d->height);
        const uint8_t *row = d->plane + (size_t)source_y * row_bytes;
        for (int c = 0; c < d->scale; c++) {
            const int source_x = vmaf_hip_ssim_symmetric_index(centre_x + c - half, (int)d->width);
            const float product = vmaf_hip_ssim_sample(d, row, source_x) * d->tap_weight;
            sum += vmaf_hip_ssim_fixed(product);
        }
    }
    /* int64 -> fp32 rounds to nearest, ties to even: the CPU's (float)sum. */
    return (float)sum * VMAF_HIP_SSIM_FIXED_INV;
}

#endif /* VMAF_SRC_FEATURE_HIP_FLOAT_SSIM_SSIM_DECIMATE_H_ */
