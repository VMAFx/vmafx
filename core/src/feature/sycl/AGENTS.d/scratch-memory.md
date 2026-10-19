---
paths:
  - core/src/feature/sycl/sycl_compat.h
  - core/src/feature/sycl/integer_adm_sycl.cpp
  - core/src/feature/sycl/integer_vif_sycl.cpp
  - core/src/feature/sycl/integer_ssim_sycl.cpp
  - core/src/feature/sycl/integer_motion_pipeline_sycl.cpp
  - core/src/feature/sycl/ssimulacra2_sycl.cpp
invariant: No scratch memory in kernels; zero private_mem_size and spill_memory_size on xe.
---
<!-- markdownlint-disable MD013 MD060 -->
# Scratch memory avoidance on Intel Xe

- **No scratch memory in kernels ([ADR-1395](../../../../../docs/adr/1395-sycl-kernels-no-scratch.md)).**
  No private array indexed at run time outside local memory, no live set above
  128 registers per thread at kernel SIMD width: Arc A-series under xe returns
  wrong values from scratch. Kernel that cannot fit -> functor derived from
  `VmafSyclKernelShape<SG, 256>` (`sycl_compat.h`, 256-entry register file;
  absent on Xe-LP, see below). `integer_vif_sycl.cpp` runs at SIMD-16 only
  ([ADR-1830](../../../../../docs/adr/1830-sycl-vif-simd16-only.md)): its
  SIMD-32 kernels and `VMAF_SYCL_VIF_SUBGROUP_SIZE` are gone; do not bring
  SIMD-32 vif path back (`test_sycl_kernel_source_contract.py` refuses it). Run
  `test_sycl_kernel_scratch` on Intel GPU after any kernel change;
  `core/src/sycl/scratch_ratchet.txt` is empty and stays empty (no kernel
  left with scratch since 2026-10-01). Smallest trap: private array indexed
  by run-time value, e.g. `pixel.original[band]` in old
  `float_adm_sycl` CM kernels (896 B private, NaN on A380 under xe) ->
  select by value: every helper of `sycl_float_adm_math.h` takes its band as
  constant, `Bands` holds CSF weights as three named fields.
- **Xe-LP has no 256-entry register file
  (T-SYCL-ROW-KERNELS-SG16-OTHER-DEVICES-2026-10-02).** On `tgllp`, `adl-*`
  and `rpl-*` `VmafSyclKernelShape<SG, 256>` still compiles at 128
  registers, so shape does not keep kernel out of scratch there (UHD
  770 spilled in 12 kernels, issues #2116 and #2122). kernel whose result
  does not depend on sub-group size leaves size to compiler:
  `VmafSyclKernelShape<0, 256>` ([ADR-1501](../../../../../docs/adr/1501-sycl-float-adm-terms-large-grf-xe2.md)),
  which icpx compiles at SIMD-8 on Xe-LP and at SIMD-16 or 32 with 256
  registers elsewhere; nothing requires 8 (ADR-1468). `Ss2SlotKernel`,
  `IssimTermKernel` and scale-0 `IntegerVifHoriKernel<0, 16>` have that
  shape, `MotionSadHbdKernel` is `<16, 0>`; do not give them required
  size back. SIMD-32 vif instances spilled on Xe-LP too and were removed
  (ADR-1830). Measure shape change on all 19 default targets
  (ocloc `.ze_info`), not on A380 alone.
