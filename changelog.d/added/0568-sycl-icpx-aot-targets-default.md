- **`sycl_icpx_aot_targets` Meson option — AOT compilation default for Intel SYCL
  ([ADR-0568](../docs/adr/0568-sycl-icpx-aot-targets-default.md)).**
  Builds with `-Denable_sycl=true` now emit a fat binary containing native GPU
  ISA blobs for 19 Intel micro-architectures (Arc A-series dGPU + every common
  iGPU from Tiger Lake through Battlemage) in addition to a SPIR-V JIT fallback
  for unlisted devices. This eliminates the several-second Level Zero / IGC
  cold-start compilation that previously hit every first kernel launch. Override
  with `-Dsycl_icpx_aot_targets='dg2-g11'` to narrow to a single-target fleet,
  or `-Dsycl_icpx_aot_targets=''` to revert to SPIR-V JIT only. The option
  mirrors `hip_gfx_targets` (ADR-0561) which applied the same default-active
  pattern to the HIP backend.
