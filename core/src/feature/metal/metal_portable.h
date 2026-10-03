/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  One spelling of the types and primitives the arithmetic headers of the
 *  Metal twins use, so that such a header compiles as Metal Shading Language
 *  in a kernel and as C or C++ on the host, where core/test holds it against
 *  the CPU extractor's reference value by value without a device (ADR-1498).
 *
 *  A header written on this prelude keeps to what the three languages share:
 *  values in and values out (no pointer or reference: MSL wants an address
 *  space on each), plain structs with `typedef`, no overloading, templates or
 *  namespaces (C), no designated initializers, no `double` (MSL has none), no
 *  `long long` or `ULL` literal (MSL has neither: write VMAF_MTL_U64(1) << 52),
 *  program-scope constants through VMAF_MTL_CONSTANT. Every operation is one
 *  of the correctly rounded fp32 operations of the Metal Shading Language
 *  Specification 4.1, Table 8.1 (+ - * / sqrt fma), under the kernels'
 *  -fno-fast-math -ffp-contract=off (core/src/metal/meson.build); the host
 *  build has the project's contraction-off floor (ADR-1461).
 */

#ifndef VMAF_FEATURE_METAL_METAL_PORTABLE_H_
#define VMAF_FEATURE_METAL_METAL_PORTABLE_H_

#if defined(__METAL_VERSION__)

#include <metal_stdlib>

typedef int vmaf_mtl_i32;
typedef uint vmaf_mtl_u32;
typedef long vmaf_mtl_i64;
typedef ulong vmaf_mtl_u64;

/* `inline` only: MSL reserves `static` for program-scope variables (4.1, 5.3). */
#define VMAF_MTL_FUNC inline
#define VMAF_MTL_CONSTANT constant
#define VMAF_MTL_U64(x) ((vmaf_mtl_u64)(x))
#define VMAF_MTL_I64(x) ((vmaf_mtl_i64)(x))
#define VMAF_MTL_FMA(a, b, c) metal::fma((a), (b), (c))
#define VMAF_MTL_SQRT(x) metal::sqrt(x)
#define VMAF_MTL_FABS(x) metal::fabs(x)
#define VMAF_MTL_F2U(x) as_type<uint>(x)
#define VMAF_MTL_U2F(x) as_type<float>(x)
/* Leading zero bits of a 64-bit value; 64 for 0. */
#define VMAF_MTL_CLZ64(x) ((vmaf_mtl_u32)metal::clz((vmaf_mtl_u64)(x)))

#else /* host C or C++ */

#include <math.h>
#include <stdint.h>
#include <string.h>

typedef int32_t vmaf_mtl_i32;
typedef uint32_t vmaf_mtl_u32;
typedef int64_t vmaf_mtl_i64;
typedef uint64_t vmaf_mtl_u64;

#define VMAF_MTL_FUNC static inline
#define VMAF_MTL_CONSTANT static const
#define VMAF_MTL_U64(x) ((vmaf_mtl_u64)(x))
#define VMAF_MTL_I64(x) ((vmaf_mtl_i64)(x))
#define VMAF_MTL_FMA(a, b, c) fmaf((a), (b), (c))
#define VMAF_MTL_SQRT(x) sqrtf(x)
#define VMAF_MTL_FABS(x) fabsf(x)

static inline uint32_t vmaf_mtl_f2u(float f)
{
    uint32_t u = 0u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static inline float vmaf_mtl_u2f(uint32_t u)
{
    float f = 0.0f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

/* Leading zero bits of a 64-bit value; 64 for 0. A bounded loop: the host
 * compilers this builds with (gcc, clang, MSVC) spell the builtin
 * differently, and the host side only checks arithmetic. */
static inline uint32_t vmaf_mtl_clz64(uint64_t x)
{
    uint32_t n = 0u;
    for (uint64_t bit = UINT64_C(1) << 63; bit != 0u && (x & bit) == 0u; bit >>= 1) {
        n++;
    }
    return n;
}

#define VMAF_MTL_F2U(x) vmaf_mtl_f2u(x)
#define VMAF_MTL_U2F(x) vmaf_mtl_u2f(x)
#define VMAF_MTL_CLZ64(x) vmaf_mtl_clz64((uint64_t)(x))

#endif

#endif /* VMAF_FEATURE_METAL_METAL_PORTABLE_H_ */
