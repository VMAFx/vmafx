**Fixed**: `test_sycl_motion_add_uv_parity` SIGSEGV on Intel Arc A380 — two root
causes resolved (ADR-1099):

1. **Missing `-fsycl` at link time** for SYCL test executables. Without this
   flag, `clang-offload-wrapper` is not invoked, the SYCL runtime's
   `ProgramManager` never registers the device kernels, and the first
   `queue.submit()` null-dereferences inside `getDeviceKernelInfo`. Fixed by
   embedding `-fsycl` in `sycl_dependency.link_args` in `core/src/meson.build`
   so every consumer — the shared library, the static library, and all SYCL test
   executables — receives the flag automatically.

2. **Wrong feature names in `vmaf_feature_score_at_index` queries**. With
   `motion_add_uv=true`, scores are stored under aliased names
   (`integer_motion2_mau`, `float_motion2_mau`) not the raw `VMAF_*_score`
   names. The test queries were updated accordingly.

The `should_fail: true` bypass (ADR-1093) is removed; the test now passes
unconditionally on SYCL-capable hardware.
