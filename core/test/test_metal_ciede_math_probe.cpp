/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-1498 — the host half of sycl_ciede_math_probe.h for the Metal twin:
 * ciede_metal's per-pixel arithmetic (feature/ciede_ff_math.h in its Metal
 * subset, on the primitives of feature/metal/metal_ciede_math.h), compiled by
 * the host compiler with the C++ library's float functions behind the
 * primitives. test_sycl_ciede_math.c, built as test_metal_ciede_math, holds
 * the pair functions to the host's extended-precision math library and the
 * pixel to the reference's fp64 statements with it. The kernel compiles the
 * same text of the shared headers (the VMAF_FF_MSL_SUBSET branches); only the
 * address-space words and the primitives' namespace differ. The constants are
 * make_constants(), which integer_ciede_metal.mm hands the kernel.
 *
 * Built a second time as test_metal_ciede_math_coarse with
 * VMAF_TEST_METAL_CIEDE_COARSE_ROOTS: the cube and fifth root estimates are
 * then 2^-18 off (Metal's exp() and log() are allowed 4 ulp each, about
 * 2^-19 of a root together), and the same bounds hold.
 *
 * Compiled with contraction off, like the kernels.
 */

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>

#if defined(VMAF_TEST_METAL_CIEDE_COARSE_ROOTS)
/* Worse estimates than the kernel's can be: 2^-18 high. */
#define VMAF_FF_CBRT(x) (std::exp(std::log(x) / 3.0f) * (1.0f + 0x1p-18f))
#define VMAF_FF_ROOT5(x) (std::exp(0.2f * std::log(x)) * (1.0f + 0x1p-18f))
#endif

#include "feature/metal/metal_ciede_math.h"
#include "sycl_ciede_math_probe.h"

namespace
{

using vmaf_ffm_base::Ff;
using vmaf_metal_ffm::kAtanTable;
using vmaf_metal_ffm::kSinCosTable;
using vmaf_metal_ffm::Tables;

/* What one evaluation returns. */
struct FfResult {
    Ff value;
    int32_t exponent;
};

/* One evaluation of pair function `function`. */
FfResult evaluate(int function, float a, float b, const Tables &tables)
{
    const Ff pair = {.hi = a, .lo = b};
    switch (function) {
    case VMAF_TEST_FF_SQRT:
        return {.value = vmaf_metal_ffm::sqrt(pair), .exponent = 0};
    case VMAF_TEST_FF_CBRT:
        return {.value = vmaf_metal_ffm::cbrt(pair), .exponent = 0};
    case VMAF_TEST_FF_POW_2_4:
        return {.value = vmaf_metal_ffm::pow_2_4(pair), .exponent = 0};
    case VMAF_TEST_FF_POW_7:
        return {.value = vmaf_metal_ffm::pow_7(a), .exponent = 0};
    case VMAF_TEST_FF_EXP: {
        const vmaf_metal_ffm::Exp e = vmaf_metal_ffm::exp(a);
        return {.value = e.value, .exponent = e.k};
    }
    case VMAF_TEST_FF_SIN:
        return {.value = vmaf_metal_ffm::sin_cos(pair, tables.sin_cos).sin, .exponent = 0};
    case VMAF_TEST_FF_COS:
        return {.value = vmaf_metal_ffm::sin_cos(pair, tables.sin_cos).cos, .exponent = 0};
    default:
        return {.value = vmaf_metal_ffm::atan2(a, b, tables.atan), .exponent = 0};
    }
}

} // namespace

extern "C" int vmaf_test_sycl_ff_batch(const VmafTestFfBatch *batch, int on_device)
{
    if (batch == nullptr || batch->a == nullptr || batch->b == nullptr || batch->hi == nullptr ||
        batch->lo == nullptr || batch->exponent == nullptr || batch->function < 0 ||
        batch->function >= VMAF_TEST_FF_COUNT) {
        return -EINVAL;
    }
    if (on_device != 0) {
        return -ENODEV;
    }
    /* As the kernel builds it: the two tables of ff_math.h. */
    const Tables tables = {.atan = kAtanTable, .sin_cos = kSinCosTable};
    for (size_t i = 0; i < batch->n; i++) {
        const FfResult r = evaluate(batch->function, batch->a[i], batch->b[i], tables);
        batch->hi[i] = r.value.hi;
        batch->lo[i] = r.value.lo;
        batch->exponent[i] = r.exponent;
    }
    return 0;
}

extern "C" int vmaf_test_sycl_ciede_batch(const VmafTestCiedeBatch *batch, int on_device)
{
    if (batch == nullptr || batch->samples == nullptr || batch->delta_e == nullptr ||
        batch->bpc < 8u || batch->bpc > 16u) {
        return -EINVAL;
    }
    if (on_device != 0) {
        return -ENODEV;
    }
    const Tables tables = {.atan = kAtanTable, .sin_cos = kSinCosTable};
    /* The constants integer_ciede_metal.mm hands the kernel for this depth. */
    const vmaf_metal_ciede::Constants k = vmaf_metal_ciede::make_constants(batch->bpc);
    for (size_t i = 0; i < batch->n; i++) {
        const float *s = batch->samples + 6 * i;
        batch->delta_e[i] = vmaf_metal_ciede::pixel({.y = s[0], .u = s[1], .v = s[2]},
                                                    {.y = s[3], .u = s[4], .v = s[5]}, k, tables);
    }
    return 0;
}
