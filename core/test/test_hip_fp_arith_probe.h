/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  The HIP device probe of test_hip_fp_arith_contract (ADR-1407).
 */

#ifndef VMAF_TEST_HIP_FP_ARITH_PROBE_H_
#define VMAF_TEST_HIP_FP_ARITH_PROBE_H_

#include <stddef.h>

/* Runs `mad[i] = a[i] * b[i] + c[i]`, `quot[i] = a[i] / b[i]` and
 * `root[i] = sqrtf(fabsf(a[i]))` for i < n on HIP device 0, in a kernel
 * compiled with the list every HIP feature kernel gets. Returns 0, -EINVAL
 * for a NULL array, -ENODEV when no HIP device is usable and -EIO when a
 * device call fails. n == 0 is a no-op. */
int vmaf_test_hip_fp_arith(const float *a, const float *b, const float *c, size_t n, float *mad,
                           float *quot, float *root);

#endif /* VMAF_TEST_HIP_FP_ARITH_PROBE_H_ */
