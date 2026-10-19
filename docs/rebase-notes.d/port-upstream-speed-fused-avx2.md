## Netflix/vmaf ad42c532 + 9cb9479f: SpEED fused anti-alias filter on x86, AVX2 vertical pass (2026-10-08)

- `core/src/feature/speed.c` `filter_and_downscale()` and its mirror
  `speed_internal_filter_and_downscale()` (`speed_internal.c`): the
  `#if ARCH_X86` branch (`vif_filter1d_s()` + `vif_dec16_s()`) is gone; every
  target calls `vif_filter1d_dec16_s()` and copies the decimated plane back.
  **On sync**: keep the two files in lockstep, as before.
- `core/src/feature/vif_tools.c`: `vif_filter1d_dec16_s()` takes its
  vertical pass from `vif_filter1d_vertical_dispatch_s()`, which calls
  `convolution_f32_avx_rows_s()` (`common/convolution_avx.c`, declared in
  `common/convolution.h`) under the same gate as `vif_filter1d_s()`'s AVX2
  convolution. Upstream's form differs on purpose:
  - upstream's `convolution_f32_avx_dec16_s()` carries both passes and a
    second scalar copy of the decimated horizontal pass; the fork vectorises
    only the vertical row and keeps one horizontal pass in `vif_tools.c`;
  - upstream's `vif_filter1d_dec16_scalar_s()` split is not taken: the CPU
    mask selects the scalar pass;
  - upstream's `VMAF_NO_FUSE` asm barrier (`convolution.h`, and in the
    existing AVX scanlines) is not taken: every translation unit builds with
    contraction off (ADR-1461).
  **On sync**: do not import those three; port a tap-order or mirror change
  into `convolution_f32_avx_rows_s()` and `vif_filter1d_vertical_s()`
  together.
- GPU twins (`cuda/speed/speed_score.cu`, `hip/speed/speed_hip_device.h`,
  `sycl/speed_sycl_pipeline.cpp`): comment lines only (same line counts).
  The Rust twin already ran the fused filter.
- `core/test/test_speed_filter.c`: SIMD and old-x86-path comparisons, an
  aligned layout (the AVX2 convolution of the old path loads aligned),
  upstream's checkasm size 79x48, and `test_avx_rows` for the masked tail
  the decimated pass never reads.
