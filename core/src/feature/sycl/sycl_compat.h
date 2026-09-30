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
 *      Expands to `[[intel::reqd_sub_group_size(N)]]` under icpx, and
 *      to nothing under AdaptiveCpp. Per the AdaptiveCpp documentation
 *      (https://adaptivecpp.github.io/AdaptiveCpp/), sub-group size is
 *      determined per backend at JIT time and a hard hint is rejected;
 *      omitting the attribute lets AdaptiveCpp pick the natural size.
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
 *
 *  Numerical impact: AdaptiveCpp output is **not** bit-identical to
 *  icpx and not bit-identical to scalar CPU. See
 *  `feedback_golden_gate_cpu_only` and ADR-0335 for the tolerance
 *  matrix.
 */

#ifndef VMAF_SRC_FEATURE_SYCL_SYCL_COMPAT_H_
#define VMAF_SRC_FEATURE_SYCL_SYCL_COMPAT_H_

#if defined(__INTEL_LLVM_COMPILER)
/* icpx / DPC++ — use the SYCL 2020 standard attribute.
 * [[intel::reqd_sub_group_size(N)]] was deprecated in oneAPI 2026.0 in
 * favour of [[sycl::reqd_sub_group_size(N)]] (supported since 2023.0). */
#define VMAF_SYCL_REQD_SG_SIZE(N) [[sycl::reqd_sub_group_size(N)]]
#elif defined(SYCL_IMPLEMENTATION_ACPP) || defined(SYCL_IMPLEMENTATION_HIPSYCL)
/* AdaptiveCpp — Intel sub-group-size attribute not supported; the
 * runtime picks the sub-group size per backend at JIT time. */
#define VMAF_SYCL_REQD_SG_SIZE(N) /* AdaptiveCpp: no-op */
#else
/* Unknown SYCL implementation — use the SYCL 2020 standard attribute;
 * if the toolchain doesn't support it, add a branch above. */
#define VMAF_SYCL_REQD_SG_SIZE(N) [[sycl::reqd_sub_group_size(N)]]
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

template <int SG_SIZE, int GRF_SIZE> struct VmafSyclKernelShape {
    static_assert(GRF_SIZE == 0 || GRF_SIZE == 256, "GRF_SIZE: 0 (device default) or 256");
#if VMAF_SYCL_KERNEL_PROPERTIES
    [[nodiscard]] auto get(sycl::ext::oneapi::experimental::properties_tag /*tag*/) const
    {
        namespace syclex = sycl::ext::oneapi::experimental;
        if constexpr (GRF_SIZE == 256) {
            return syclex::properties{syclex::sub_group_size<SG_SIZE>,
                                      sycl::ext::intel::experimental::grf_size<256>};
        } else {
            return syclex::properties{syclex::sub_group_size<SG_SIZE>};
        }
    }
#endif
};

#endif /* VMAF_SRC_FEATURE_SYCL_SYCL_COMPAT_H_ */
