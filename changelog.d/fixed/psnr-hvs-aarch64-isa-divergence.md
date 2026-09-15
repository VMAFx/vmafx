- **`psnr_hvs` no longer scores differently with and without NEON on aarch64.**
  The ADR-1207 ISA-invariance gate reported host-isa `14.191670308986598`
  against scalar `14.191669969203765`, a delta of **3.398e-07**, on the
  `Ubuntu ARM clang` lane. Both SIMD twins are already built
  `-ffp-contract=off` — `x86_psnr_hvs_avx2_lib` and the arm64 `arm64_fp_lib` —
  but the scalar reference they are compared against, the vendored
  `third_party/xiph/psnr_hvs.c`, was not, so clang was free to contract
  `a + b * c` in it. It only surfaced on aarch64: FMA is baseline there, while
  on x86-64 there is no FMA without `-mfma`, so nothing contracted and the two
  paths happened to agree. The scalar file now carries the same contract policy
  as its twins, applied in `meson.build` rather than in the vendored source so
  the next upstream sync stays a clean diff. x86 codegen is unchanged — compiled
  both ways, the instruction streams are identical at 889 instructions with zero
  FMA. Reproduced and verified under `qemu-aarch64-static` with a clang cross
  build.
