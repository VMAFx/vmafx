/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Host side of test_sycl_speed_cov_chain: the split covariance of
 *  feature/sycl/sycl_speed_cov_math.h (ADR-2690) as speed_sycl_pipeline.cpp
 *  runs it, on the host. core/test/meson.build compiles this TU with the SYCL
 *  feature line, so the arithmetic is built like the extractor's. No kernel
 *  is launched: the test is device-free.
 */

#include <cstddef>
#include <cstdint>

#include <sycl/sycl.hpp>

#include "../src/feature/sycl/sycl_compat.h"
#include "../src/feature/sycl/sycl_speed_cov_math.h"

namespace
{

namespace cov = vmaf_sycl_speed_cov;
using vmaf_sycl_soft::signed_bits;
using vmaf_sycl_soft::signed_from_bits;
using vmaf_sycl_soft::signed_from_float;
using vmaf_sycl_soft::SoftSigned;

/* The two difference planes of one entry, as the difference kernel stores
 * them: `dx[k]`, `dy[k]` for pixel k of the submatrix in raster order. */
void store_differences(const float *x, const float *y, size_t stride, uint32_t width,
                       uint32_t height, float mean_x, float mean_y, uint64_t *dx, uint64_t *dy)
{
    const SoftSigned mx = signed_from_float(mean_x);
    const SoftSigned my = signed_from_float(mean_y);
    for (uint32_t i = 0; i < height; i++) {
        for (uint32_t j = 0; j < width; j++) {
            const size_t k = static_cast<size_t>(i) * width + j;
            const size_t at = static_cast<size_t>(i) * stride + j;
            dx[k] = signed_bits(cov::covariance_difference(x[at], mx));
            dy[k] = signed_bits(cov::covariance_difference(y[at], my));
        }
    }
}

} // namespace

/* signed_bits(signed_add(a, b)) for two normal fp64 bit patterns (or +0). */
extern "C" uint64_t vmaf_test_cov_soft_add(uint64_t a, uint64_t b)
{
    return signed_bits(cov::covariance_chain_add(signed_from_bits(a), b));
}

/* signed_bits(signed_from_bits(bits)): the storage of a term or a sum. */
extern "C" uint64_t vmaf_test_cov_bits_round_trip(uint64_t bits)
{
    return signed_bits(signed_from_bits(bits));
}

/* covariance_entry() (A), on the host. */
extern "C" float vmaf_test_cov_entry_a(const float *x, const float *y, size_t stride,
                                       uint32_t width, uint32_t height, float mean_x, float mean_y)
{
    return cov::covariance_entry(x, y, stride, width, height, mean_x, mean_y);
}

/* The split form of the same entry, as the pipeline runs it: the differences
 * and the terms stored as fp64 bit patterns, then the chain over the terms in
 * slices of `slice_rows` submatrix rows, the running sum handed from slice to
 * slice as its bit pattern, then the store. `scratch` holds 3 * width *
 * height words. */
extern "C" float vmaf_test_cov_split_entry(const float *x, const float *y, size_t stride,
                                           uint32_t width, uint32_t height, float mean_x,
                                           float mean_y, uint32_t slice_rows, uint64_t *scratch)
{
    const size_t n = static_cast<size_t>(width) * height;
    uint64_t *dx = scratch;
    uint64_t *dy = scratch + n;
    uint64_t *terms = scratch + 2 * n;
    store_differences(x, y, stride, width, height, mean_x, mean_y, dx, dy);
    for (size_t k = 0; k < n; k++) {
        terms[k] =
            signed_bits(cov::covariance_term(signed_from_bits(dx[k]), signed_from_bits(dy[k])));
    }
    uint64_t sum_bits = signed_bits(cov::covariance_zero());
    for (uint32_t row = 0; row < height; row += slice_rows) {
        const uint32_t rows = height - row < slice_rows ? height - row : slice_rows;
        const SoftSigned sum = cov::covariance_chain(
            signed_from_bits(sum_bits), terms + static_cast<size_t>(row) * width, 1u, rows * width);
        sum_bits = signed_bits(sum);
    }
    return cov::covariance_store(signed_from_bits(sum_bits), n);
}
