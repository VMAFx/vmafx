---
paths:
  - core/src/feature/sycl/sycl_compat.h
  - core/src/feature/sycl/float_motion_sycl.cpp
  - core/src/feature/sycl/float_adm_sycl.cpp
invariant: Kernel sub-group size: 16 or 32 only (ADR-1468); Xe2 AOT targets reject 8.
---
<!-- markdownlint-disable MD013 MD060 -->
# Kernel sub-group size policy

- **Kernel sub-group size: 16 or 32 only (ADR-1468).** Xe2 AOT targets
  reject 8; `sycl_compat.h` static_asserts it. The row kernels
  (`launch_float_motion_row_sad`, `float_adm` `launch_row_sums`,
  `launch_vif_row_sums`) and the `ssimulacra2` walk (`SS2S_WALK_SG`)
  required 8 for speed and are at 16: same bits (each is one sequential
  loop per work-item), scratch-free on A380, <= 2 % slower at 4K. Never
  restore 8 "for the A380": the default build compiles for 19 targets.
  Guards and the suite to run: [../../sycl/AGENTS.md](../../../sycl/AGENTS.md).
  Verified at 16 on an Arc B580 (Xe2): same bits, no scratch (2026-10-03).
  Unverified on Xe-LP devices:
  `T-SYCL-ROW-KERNELS-SG16-OTHER-DEVICES-2026-10-02`.
- **`VmafSyclKernelShape<0, 256>` = no required size + large GRF
  (ADR-1501).** Only with GRF 256 (`static_assert`); for a kernel that fits
  Xe-LP only at the compiler's SIMD-8 but spills at Xe2's SIMD-32
  (`float_adm` term kernel). No `VMAF_SYCL_FUNCTOR_SG_SIZE` on such a
  functor. The contract resolves `ns::kConstant` shape arguments across the
  SYCL sources.
