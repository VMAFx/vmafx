/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The covariance entry speed_chroma_sycl / speed_temporal_sycl compute
 * against speed.c's own compute_cov_kernel_scalar(), on the host and on the
 * device (T-SPEED-CHROMA-SYCL-COV-1ULP-2026-10-06).
 *
 * The reference adds `(x - mean_x) * (y - mean_y)` into one fp64 running sum
 * in raster order, rounding every add, and stores (float)(sum / (w * h)).
 * The twin once summed the same terms near-exactly in parallel and rounded
 * once; on an entry whose terms cancel the two differ by ~1e-14 of the sum,
 * and now and then the stored fp32 value by one step (on a real
 * 3840x1600 10-bit frame, speed_chroma_u). This test builds blocks whose
 * terms cancel to a few parts in 1e7 of their magnitude, keeps the blocks
 * where a near-exact sum (long double, rounded once) stores another fp32
 * value than the reference does, and requires covariance_entry() to return
 * the reference's value with `==` on every block, flat or cancelling:
 *
 *   - on the host;
 *   - in a kernel on the default GPU;
 *   - split as the pipeline runs it (ADR-2690): differences, products and the
 *     add chain in three kernels over stored fp64 bit patterns, on that GPU.
 *
 * It also requires that enough blocks discriminate: a fixture that the old
 * parallel design passes would test nothing.
 *
 * Skip behaviour: the host check always runs; without a SYCL GPU the device
 * check is skipped and the test exits 77.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/speed_cov.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

void vmaf_test_sycl_cov_host(const float *x, const float *y, const float *mean_x,
                             const float *mean_y, size_t n, size_t stride, uint32_t width,
                             uint32_t height, float *out);
int vmaf_test_sycl_cov_device(const float *x, const float *y, const float *mean_x,
                              const float *mean_y, size_t n, size_t stride, uint32_t width,
                              uint32_t height, float *out);
int vmaf_test_sycl_cov_split_device(const float *x, const float *y, const float *mean_x,
                                    const float *mean_y, size_t n, size_t stride, uint32_t width,
                                    uint32_t height, float *out);

#include "speed_cov_cases.h"

static char *test_fixture_separates_the_two_sums(void)
{
    build_cases();
    (void)fprintf(stderr, "[%u of %d blocks separate the sums] ", discriminating, CASES);
    mu_assert("too few blocks where the reference's sum and a near-exact sum store different "
              "fp32 values: the fixture would not catch a parallel exact sum",
              discriminating >= MIN_DISCRIMINATING);
    return NULL;
}

static char *test_host_entry_is_the_reference(void)
{
    build_cases();
    vmaf_test_sycl_cov_host(xs, ys, mxs, mys, CASES, STRIDE, WIDTH, HEIGHT, got);
    mu_assert("the covariance entry differs from compute_cov_kernel_scalar() on the host",
              count_wrong("host") == 0u);
    return NULL;
}

static char *test_device_entry_is_the_reference(void)
{
    build_cases();
    const int err = vmaf_test_sycl_cov_device(xs, ys, mxs, mys, CASES, STRIDE, WIDTH, HEIGHT, got);
    if (err == -ENODEV) {
        (void)fprintf(stderr, "[skip: no SYCL GPU] ");
        mu_skipped = 1;
        return NULL;
    }
    mu_assert("the device covariance kernel failed", err == 0);
    mu_assert("the covariance entry differs from compute_cov_kernel_scalar() on the device",
              count_wrong("device") == 0u);
    return NULL;
}

/* The split form the pipeline runs (ADR-2690), in three kernels on the GPU. */
static char *test_device_split_is_the_reference(void)
{
    build_cases();
    const int err =
        vmaf_test_sycl_cov_split_device(xs, ys, mxs, mys, CASES, STRIDE, WIDTH, HEIGHT, got);
    if (err == -ENODEV) {
        (void)fprintf(stderr, "[skip: no SYCL GPU] ");
        mu_skipped = 1;
        return NULL;
    }
    mu_assert("the device split covariance failed", err == 0);
    mu_assert("the split covariance differs from compute_cov_kernel_scalar() on the device",
              count_wrong("device split") == 0u);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_fixture_separates_the_two_sums);
    mu_run_test(test_host_entry_is_the_reference);
    mu_run_test(test_device_entry_is_the_reference);
    mu_run_test(test_device_split_is_the_reference);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
