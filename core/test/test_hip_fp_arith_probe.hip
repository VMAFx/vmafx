/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Device side of test_hip_fp_arith_contract (ADR-1407). core/test/meson.build
 *  compiles this file with hipcc and `hip_strict_fp_args`, the list every
 *  feature kernel gets, so the kernel below is built exactly like an
 *  extractor's.
 */

#include <hip/hip_runtime.h>

extern "C" {

/* One multiply-add written as a single expression, one division and one
 * square root per element: the three operations hipcc changes on the device
 * when the strict FP list is incomplete. */
__global__ void vmaf_test_hip_fp_arith_kernel(const float *__restrict__ a,
                                              const float *__restrict__ b,
                                              const float *__restrict__ c, float *__restrict__ mad,
                                              float *__restrict__ quot, float *__restrict__ root,
                                              unsigned n)
{
    const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n)
        return;
    mad[i] = a[i] * b[i] + c[i];
    quot[i] = a[i] / b[i];
    root[i] = sqrtf(fabsf(a[i]));
}

} /* extern "C" */
