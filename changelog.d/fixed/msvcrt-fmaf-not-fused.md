
- **`ssimulacra2` and `float_ms_ssim` scored differently with and without SIMD
  on Windows.** libm's `fmaf()` is a genuine fused multiply-add on glibc, musl
  and the UCRT, but not on the legacy `msvcrt.dll` that MSYS2's `MINGW64`
  environment links — the environment the `Windows MinGW64` lane builds in.
  Two scalar references called it to hold the ADR-0891 single-rounding contract
  against SIMD twins that fuse with `_mm256_fmadd_ps`, so on that lane the
  scalar path rounded twice and the SIMD path once: `ssimulacra2` came out
  0.37 points apart on a 48-frame clip, `float_ms_ssim` 2.4e-7. Both call sites
  now use `vmaf_fmaf_exact()`, which evaluates the product and sum in `double`
  and rounds once — the correctly-rounded fused result on every host, with no
  FMA hardware and no libm call. Verified bit-identical to glibc `fmaf` and to
  the `vfmadd213ss` instruction over 20 million random triples; every Linux
  pooled score is unchanged to the last bit. Pre-existing on master and found
  by ADR-1207's ISA-invariance gate. See ADR-1253.
