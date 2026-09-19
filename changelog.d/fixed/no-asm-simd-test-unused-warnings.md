**Cleared `-Wunused-function` / `-Wunused-const-variable` / `-Wunused-variable`
warnings in a `-Denable_asm=false` build** — ten SIMD parity-test files under
`core/test/` defined scalar-reference helpers, fixture builders, and lookup
tables unconditionally while every caller sat under an ISA guard (`ARCH_X86`,
`HAVE_AVX512`, `ARCH_AARCH64`, or their union). A build with SIMD disabled
compiled the helpers in with zero remaining callers and warned on all ~30 of
them: `test_vif_simd.c`, `test_ssimulacra2_simd.c`, `test_speed_simd.c`,
`test_psnr_hvs_simd.c`, `test_ms_ssim_decimate.c`, `test_motion_v2_simd.c`,
`test_iqa_convolve.c`, `test_integer_ssim_simd.c`, `test_cambi_simd.c`, and
`test_cambi.c`. Each helper now sits under the same preprocessor condition as
its callers (widened to the union where a helper is shared across the x86 and
aarch64 paths); nothing was deleted and no `(void)`-cast or
`[[maybe_unused]]` suppression was used. Verified zero warnings and an
identical test count on a `-Denable_asm=false` x86-64 build, the default
x86-64 build, and an aarch64 cross build under `qemu-aarch64-static`.
