/**
 *
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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
 *  Numerical impact: AdaptiveCpp output is **not** bit-identical to
 *  icpx and not bit-identical to scalar CPU. See
 *  `feedback_golden_gate_cpu_only` and ADR-0335 for the tolerance
 *  matrix.
 */

#ifndef VMAF_SRC_FEATURE_SYCL_SYCL_COMPAT_H_
#define VMAF_SRC_FEATURE_SYCL_SYCL_COMPAT_H_

#if defined(__INTEL_LLVM_COMPILER)
/* icpx / DPC++ — emit the Intel-specific attribute verbatim. */
#define VMAF_SYCL_REQD_SG_SIZE(N) [[intel::reqd_sub_group_size(N)]]
#elif defined(SYCL_IMPLEMENTATION_ACPP) || defined(SYCL_IMPLEMENTATION_HIPSYCL)
/* AdaptiveCpp — Intel sub-group-size attribute not supported; the
 * runtime picks the sub-group size per backend at JIT time. */
#define VMAF_SYCL_REQD_SG_SIZE(N) /* AdaptiveCpp: no-op */
#else
/* Unknown SYCL implementation — assume the Intel attribute is
 * harmless to emit; if it isn't, the new toolchain owner adds a
 * branch above. */
#define VMAF_SYCL_REQD_SG_SIZE(N) [[intel::reqd_sub_group_size(N)]]
#endif

#endif /* VMAF_SRC_FEATURE_SYCL_SYCL_COMPAT_H_ */
