### Fixed

Round-3 bug-hunt — build/GPU error-path batch (golden-safe).

- **R3-6** HIP `integer_vif` init returned an **uninitialized** error code
  (`core/src/feature/hip/integer_vif_hip.c`): the `fail_stream`/`fail_submit`
  goto paths reached `return err;` before `int err` was declared (it was
  declared at the `vif_hip_module_load` call). Declare `int err = 0;` at the
  top of `init_fex_hip` and drop the later redeclaration.
- **R3-9** NVTX dependency linked an **empty** `shared_library('dl')` target
  instead of the system libdl (`core/src/meson.build`): replaced with
  `cc.find_library('dl', required: false)`, mirroring how `ze_loader`/`m`/etc.
  are resolved.
- **R3-10** icx strict-FP flag missing on the SSIM AVX2 carve-out
  (`core/src/meson.build` `x86_ssim_avx2_lib`): appended
  `_x86_simd_strict_fp_extra` (`-fp-model=precise` under intel-llvm) so SSIM
  AVX2 matches the scalar reference under icx, matching its sibling carve-outs.
  No-op on gcc/clang (`_x86_simd_strict_fp_extra` is empty) → golden gate
  unchanged.
