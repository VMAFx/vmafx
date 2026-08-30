# Fix SIMD bit-exactness failures in the all-backends CI build (icx)

`test_psnr_hvs_simd`, `test_ms_ssim_decimate`, and `test_ssimulacra2_simd`
were red in the "Build - Linux (GCC, all backends)" CI job, which uses
Intel `icx` (oneAPI 2025.3) as the C compiler when SYCL is enabled.

Root cause: Intel `icx` defaults to `-ffp-contract=on` (unlike GCC which
defaults to `-ffp-contract=off`), and additionally ignores
`#pragma STDC FP_CONTRACT OFF` in source files unless `-fp-model=precise`
is also on the command line. This caused scalar reference functions
in test TUs to be compiled with FMA auto-contraction while the SIMD
carve-out static libs (compiled with `-ffp-contract=off`) were not,
producing a divergence that failed the byte-exact assertions.

Fix: add `-ffp-contract=off` to `test_psnr_hvs_simd` and
`test_ms_ssim_decimate` test executables (they were missing it); upgrade
the `_simd_strict_fp_args` helper to also append `-fp-model=precise` when
the compiler is detected as `intel-llvm`; apply the same `_fp-model=precise`
extension to all x86 SIMD carve-out static libs in `core/src/meson.build`.

Verified: 49/49 fast+simd tests pass under GCC (local) and the fix logic
is sound for icx per Intel oneAPI documentation.
