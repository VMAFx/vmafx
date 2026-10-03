/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  ciede2000 for Metal kernels: feature/ciede_ff_math.h, the per-pixel
 *  arithmetic of ciede.c in fp32 pairs that the SYCL and HIP twins run
 *  (ADR-1436, ADR-1448), on the primitives of a Metal device (ADR-1498). The
 *  arithmetic and the pair functions are in the shared headers; this one names
 *  what they are built on here:
 *
 *    - the exact pair operations of feature/ff_pair.h: fp32 `+ - * /` and
 *      sqrt() are correctly rounded under the kernels' -fno-fast-math
 *      -ffp-contract=off (core/src/metal/meson.build; Metal Shading Language
 *      Specification 4.1, Table 8.1), and metal::fma() is one rounding;
 *    - exp(log(x) / 3) and exp(0.2 log(x)) as the cube and fifth root
 *      estimates the pair functions correct (Metal has no cbrt()). Metal's
 *      exp() and log() are within 4 ulp each, which puts an estimate within
 *      about 2^-19 of the root; ff_math.h needs 2^-16;
 *    - rint(), fabs() and ldexp(), which are exact or correctly rounded.
 *
 *  Metal's language revision is C++14-based, has no fp64 type and wants an
 *  address space on every reference and program-scope variable, so the shared
 *  headers are compiled in their Metal subset (VMAF_FF_MSL_SUBSET): positional
 *  initializers, the constants and the two tables of ff_math.h in the
 *  constant address space, the Constants and Tables the functions take by
 *  reference in the calling thread's, and no make_constants() in a kernel.
 *  The twin's host (integer_ciede_metal.mm) evaluates make_constants() for the
 *  frame's bit depth and hands the kernel the result.
 *
 *  The host side of this header is the same subset with the C++ library's
 *  float functions behind the primitives: integer_ciede_metal.mm takes
 *  make_constants() from it, and core/test/test_metal_ciede_math_probe.cpp
 *  holds the pair functions and the pixel to the reference's fp64 statements
 *  with it, without a device. A translation unit that includes it is compiled
 *  with contraction off; the host side needs C++20 (make_constants()).
 */

#ifndef VMAF_FEATURE_METAL_METAL_CIEDE_MATH_H_
#define VMAF_FEATURE_METAL_METAL_CIEDE_MATH_H_

#define VMAF_FF_MSL_SUBSET 1

#if defined(__METAL_VERSION__)

#include "metal_portable.h"

#define VMAF_FF_INLINE VMAF_MTL_FUNC
#define VMAF_FF_PROGRAM_CONST VMAF_MTL_CONSTANT
#define VMAF_FF_TABLE_SPACE constant
#define VMAF_FF_REF_SPACE thread
#define VMAF_FF_FMA(a, b, c) VMAF_MTL_FMA(a, b, c)
#define VMAF_FF_FABS(x) VMAF_MTL_FABS(x)
#define VMAF_FF_SQRT(x) VMAF_MTL_SQRT(x)
#define VMAF_FF_RINT(x) metal::rint(x)
#define VMAF_FF_CBRT(x) metal::exp(metal::log(x) / 3.0f)
#define VMAF_FF_ROOT5(x) metal::exp(0.2f * metal::log(x))
#define VMAF_FF_LDEXP(x, k) metal::ldexp((x), (k))

#else /* host C++ */

#include <cmath>

#define VMAF_FF_INLINE inline
#define VMAF_FF_PROGRAM_CONST constexpr
#define VMAF_FF_TABLE_SPACE
#define VMAF_FF_REF_SPACE
#define VMAF_FF_FMA(a, b, c) std::fma((a), (b), (c))
#define VMAF_FF_FABS(x) std::fabs(x)
#define VMAF_FF_SQRT(x) std::sqrt(x)
#define VMAF_FF_RINT(x) std::rint(x)
#define VMAF_FF_LDEXP(x, k) std::ldexp((x), (k))
/* A host test may define coarser estimates first
 * (test_metal_ciede_math_coarse). */
#if !defined(VMAF_FF_CBRT)
#define VMAF_FF_CBRT(x) std::exp(std::log(x) / 3.0f)
#endif
#if !defined(VMAF_FF_ROOT5)
#define VMAF_FF_ROOT5(x) std::exp(0.2f * std::log(x))
#endif

#endif

#include "../ff_pair.h"

namespace vmaf_ffm_base = vmaf_ff_pair;

#include "../ciede_ff_math.h"

namespace vmaf_metal_ciede = vmaf_ciede_ff;
namespace vmaf_metal_ffm = vmaf_ffm;

/* The host hands the kernel one Constants through setBytes(): the two sides
 * must lay it out alike, 79 floats without padding. */
static_assert(sizeof(vmaf_metal_ciede::Constants) == 79u * sizeof(float),
              "metal_ciede_math.h: Constants is not 79 packed floats");

#endif /* VMAF_FEATURE_METAL_METAL_CIEDE_MATH_H_ */
