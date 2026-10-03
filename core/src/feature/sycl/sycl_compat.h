/**
 *
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Toolchain-portable shims for the fork's SYCL feature kernels.
 *
 *  The fork's SYCL backend has historically required Intel oneAPI
 *  (`icpx`); ADR-0335 adds AdaptiveCpp (formerly OpenSYCL / hipSYCL)
 *  as a second supported toolchain. AdaptiveCpp does not implement
 *  Intel-specific extensions such as `[[intel::reqd_sub_group_size(N)]]`,
 *  so kernel code that previously hard-coded those attributes needs to
 *  reduce to a no-op when compiled by `acpp`.
 *
 *  This header is consumed by every SYCL feature TU. Its only public
 *  surface is the macros below; everything else stays a kernel-author
 *  contract.
 *
 *  ## Compiler identification
 *
 *    - icpx / DPC++:     defines `__INTEL_LLVM_COMPILER`.
 *    - AdaptiveCpp:      <sycl/sycl.hpp> defines `SYCL_IMPLEMENTATION_ACPP`
 *                        (and the legacy `SYCL_IMPLEMENTATION_HIPSYCL`).
 *
 *  Both macros are set by the SYCL implementation header; this file
 *  must be included **after** `<sycl/sycl.hpp>` for the AdaptiveCpp
 *  detection to fire.
 *
 *  ## Macros
 *
 *    VMAF_SYCL_REQD_SG_SIZE(N)
 *      Expands to `[[sycl::reqd_sub_group_size(N)]]` under icpx, and
 *      to nothing under AdaptiveCpp. Per the AdaptiveCpp documentation
 *      (https://adaptivecpp.github.io/AdaptiveCpp/), sub-group size is
 *      determined per backend at JIT time and a hard hint is rejected;
 *      omitting the attribute lets AdaptiveCpp pick the natural size.
 *      N must be 16 or 32 (VmafSyclSubGroupSize, ADR-1468): the sizes
 *      every target of the default AOT list accepts. A kernel that
 *      requires 8 does not compile for Xe2 (lnl-m, bmg-g21, bmg-g31),
 *      and one such kernel fails its whole translation unit.
 *
 *    VmafSyclKernelShape<SG, GRF>  (ADR-1395)
 *      Base class for a kernel functor that needs the large register
 *      file. Under icpx it gives the functor get(properties_tag), which
 *      returns sub_group_size<SG> and, when GRF is 256,
 *      sycl_ext_intel_grf_size's grf_size<256>: each hardware thread then
 *      holds 256 GRF registers instead of 128, so a kernel whose values
 *      do not fit at SIMD-32 stays in registers instead of spilling to
 *      scratch memory. GRF 0 keeps the device default. Under other
 *      toolchains the base is empty and the functor's call operator
 *      carries VMAF_SYCL_FUNCTOR_SG_SIZE(SG), which expands to the
 *      sub-group attribute where VMAF_SYCL_REQD_SG_SIZE does.
 *      SG 0 (with GRF 256 only) requires no sub-group size: the compiler
 *      picks one per target, with the large register file where the
 *      target has one (ADR-1501). For a kernel that fits SIMD-8 on Xe-LP,
 *      which has no large register file, but spills at the SIMD-32 icpx
 *      picks for Xe2. Its call operator carries no sub-group attribute.
 *
 *    VMAF_SYCL_ALWAYS_INLINE  (ADR-1395)
 *      `inline` plus the always-inline attribute, for header functions a
 *      kernel calls: a call left in a kernel is a scratch-memory frame.
 *
 *  Numerical impact: AdaptiveCpp output is **not** bit-identical to
 *  icpx and not bit-identical to scalar CPU. See
 *  `feedback_golden_gate_cpu_only` and ADR-0335 for the tolerance
 *  matrix.
 */

#ifndef VMAF_SRC_FEATURE_SYCL_SYCL_COMPAT_H_
#define VMAF_SRC_FEATURE_SYCL_SYCL_COMPAT_H_

/* ADR-1468: the sub-group sizes a kernel may require. icpx compiles every
 * kernel ahead of time for each target of sycl_icpx_aot_targets
 * (core/meson_options.txt), and a target that does not support a required
 * size fails the kernel, which fails the translation unit and the build:
 *
 *     [lnl-m] error: in kernel '...': Kernel compiled with required subgroup
 *     size 8, which is unsupported on this platform
 *
 * Measured with ocloc 26.35 (`intel_reqd_sub_group_size` on a one-line
 * kernel, per target): tgllp, adl-*, rpl-*, dg2-*, acm-*, mtl-* and arl-*
 * accept 8, 16 and 32; lnl-m, bmg-g21 and bmg-g31 (Xe2) and ptl-h (Xe3)
 * accept 16 and 32 only. 16 and 32 are the sizes every listed target takes.
 * The check is in the type, so a build that compiles for one device or for
 * none (a JIT-only build) rejects another size too.
 * core/test/test_sycl_sub_group_size_contract.py repeats the measurement
 * where ocloc is installed. */
template <int N> struct VmafSyclSubGroupSize {
    static_assert(N == 16 || N == 32, "a required sub-group size must be 16 or 32: Xe2 targets of "
                                      "the default AOT list accept no other (ADR-1468)");
    static constexpr int value = N;
};

#if defined(__INTEL_LLVM_COMPILER)
/* icpx / DPC++ — use the SYCL 2020 standard attribute.
 * [[intel::reqd_sub_group_size(N)]] was deprecated in oneAPI 2026.0 in
 * favour of [[sycl::reqd_sub_group_size(N)]] (supported since 2023.0). */
#define VMAF_SYCL_REQD_SG_SIZE(N) [[sycl::reqd_sub_group_size(VmafSyclSubGroupSize<N>::value)]]
#elif defined(SYCL_IMPLEMENTATION_ACPP) || defined(SYCL_IMPLEMENTATION_HIPSYCL)
/* AdaptiveCpp — Intel sub-group-size attribute not supported; the
 * runtime picks the sub-group size per backend at JIT time. */
#define VMAF_SYCL_REQD_SG_SIZE(N) /* AdaptiveCpp: no-op */
#else
/* Unknown SYCL implementation — use the SYCL 2020 standard attribute;
 * if the toolchain doesn't support it, add a branch above. */
#define VMAF_SYCL_REQD_SG_SIZE(N) [[sycl::reqd_sub_group_size(VmafSyclSubGroupSize<N>::value)]]
#endif

/* ADR-1395: kernel properties (sub-group size, register file size) come
 * from a functor's get(properties_tag) under icpx. Passing them to
 * parallel_for as an argument is deprecated in oneAPI 2026.0, and a kernel
 * that has both the sub-group attribute and properties draws
 * -Wignored-attributes, so a functor derived from VmafSyclKernelShape
 * states its sub-group size in the properties only. */
#if defined(__INTEL_LLVM_COMPILER) && __has_include(<sycl/ext/intel/experimental/grf_size_properties.hpp>)
#include <sycl/ext/intel/experimental/grf_size_properties.hpp>
#define VMAF_SYCL_KERNEL_PROPERTIES 1
#define VMAF_SYCL_FUNCTOR_SG_SIZE(N) /* in VmafSyclKernelShape::get() */
#else
#define VMAF_SYCL_KERNEL_PROPERTIES 0
#define VMAF_SYCL_FUNCTOR_SG_SIZE(N) VMAF_SYCL_REQD_SG_SIZE(N)
#endif

/* VMAF_SYCL_ALWAYS_INLINE: for a function a kernel calls many times per
 * work-item (the pair functions of sycl_ff_math.h, the integer fp64
 * operations of sycl_soft_signed.h). Left to its own judgement the compiler
 * keeps some of them as calls, and a call inside a kernel takes its frame from
 * scratch memory, which returns wrong values on Arc A-series under the xe
 * driver (ADR-1395). */
#if defined(__GNUC__) || defined(__clang__)
#define VMAF_SYCL_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define VMAF_SYCL_ALWAYS_INLINE inline
#endif

/* The sub-group size a kernel shape requires: SG_SIZE, checked by
 * VmafSyclSubGroupSize; 0 requires none (ADR-1501). */
template <int SG_SIZE> struct VmafSyclShapeSubGroup : VmafSyclSubGroupSize<SG_SIZE> {};
template <> struct VmafSyclShapeSubGroup<0> {
    static constexpr int value = 0;
};

template <int SG_SIZE, int GRF_SIZE> struct VmafSyclKernelShape {
    static_assert(GRF_SIZE == 0 || GRF_SIZE == 256, "GRF_SIZE: 0 (device default) or 256");
    static_assert(SG_SIZE != 0 || GRF_SIZE == 256,
                  "SG_SIZE 0 (no required sub-group size) only with GRF_SIZE 256 (ADR-1501)");
    static constexpr int sub_group_size = VmafSyclShapeSubGroup<SG_SIZE>::value;
#if VMAF_SYCL_KERNEL_PROPERTIES
    [[nodiscard]] auto get(sycl::ext::oneapi::experimental::properties_tag /*tag*/) const
    {
        namespace syclex = sycl::ext::oneapi::experimental;
        if constexpr (SG_SIZE == 0) {
            return syclex::properties{sycl::ext::intel::experimental::grf_size<256>};
        } else if constexpr (GRF_SIZE == 256) {
            return syclex::properties{syclex::sub_group_size<sub_group_size>,
                                      sycl::ext::intel::experimental::grf_size<256>};
        } else {
            return syclex::properties{syclex::sub_group_size<sub_group_size>};
        }
    }
#endif
};

#endif /* VMAF_SRC_FEATURE_SYCL_SYCL_COMPAT_H_ */
