/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Device side of test_hip_float_adm_math (ADR-1458). core/test/meson.build
 *  compiles this file with hipcc and `hip_strict_fp_args`, the list every
 *  feature kernel gets, so the arithmetic below is built exactly like
 *  float_adm_hip's.
 */

#include <hip/hip_runtime.h>
#include <cstddef>
#include <cstdint>

#include "feature/hip/float_adm/float_adm_hip_math.h"

#include "hip_float_adm_math_sample.h"

extern "C" {

/* One sample per thread through fadm_probe_sample(). */
__global__ void vmaf_test_hip_float_adm_kernel(const FadmProbeArgs a, const float *in, float *out,
                                               unsigned n)
{
    const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n)
        return;
    fadm_probe_sample(&a, in + (size_t)i * FADM_PROBE_IN, out + (size_t)i * FADM_PROBE_OUT);
}

} /* extern "C" */
