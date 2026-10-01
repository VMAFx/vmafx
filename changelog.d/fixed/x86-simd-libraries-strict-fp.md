- **The CPU extractors of an Intel-compiler build match a GCC build where
  they use SIMD.** The two general x86 SIMD libraries were built without
  `-ffp-contract=off`, so `icx` (which every SYCL build uses) turned plain-C
  arithmetic in the tails of SIMD kernels into fused multiply-adds. In
  `ssim_avx512.c` that moved a score: on an AVX-512 host the CPU
  `float_ms_ssim` of an icx build differed from a GCC build and from its own
  scalar path by one fp32 unit in a per-scale mean, 7.7e-9 to 1.4e-8 in the
  score, on 4 of 104 frames (Netflix 576x324 pair, 1080p checkerboards, BBB
  3840x2160). `adm`, `vif` and `speed` files were contracted too, with no
  score difference measured. Every x86 SIMD library is now built with the
  strict floating-point flags
  ([ADR-1415](docs/adr/1415-x86-simd-libraries-strict-fp.md)). GCC builds
  are unchanged: the disassembly of all 28 objects is identical with and
  without the flag. `test_ssim_x86_simd` compares the AVX2 and AVX-512 SSIM
  kernels with the scalar reference bit for bit, and `test_integer_adm_simd`,
  which failed on icx builds since #1700 because its own translation unit
  carried no FP flag, passes again. An icx build still differs from a GCC
  build in `psnr`, `psnr_hvs`, `ciede` and `speed_chroma` (at most 7.1e-15,
  7.1e-15, 5.7e-12 and 1.2e-6) because it calls Intel's math library.
