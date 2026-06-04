- Fix Go CI RED: `-Denable_metal=false` changed to `-Denable_metal=disabled`
  (meson `feature` options require string values `enabled`/`disabled`/`auto`, not
  booleans). Fixes the libvmaf meson-setup step that generates CGo bindings (ADR-1041).
- Fix Rust CI RED: `test_motion_avx512_parity` build and test registration now gated
  on `is_avx512_enabled` so the test is skipped when `enable_avx512=false`, preventing
  unresolved-symbol linker failures (ADR-1041).
