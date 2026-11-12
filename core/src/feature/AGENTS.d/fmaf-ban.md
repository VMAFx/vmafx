---
paths:
  - core/src/feature/feature_extractor.h
invariant: Scalar references never call libm fmaf to prevent contraction divergence across hosts.
area: framework
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Scalar Reference libm fmaf Prohibition

- **Scalar references never call libm `fmaf()`** (ADR-1253). Where scalar
  reference must be bit-exact with SIMD kernel that fuses — `_mm256_fmadd_ps`,
  `vfmla` — it calls `vmaf_fmaf_exact()` from
  [`common/fmaf_exact.h`](../common/fmaf_exact.h), which evaluates product and
  sum in `double` and rounds once. `fmaf()` is genuine fused multiply-add on
  glibc, musl and UCRT but **not** on legacy `msvcrt.dll` that MSYS2's
  `MINGW64` links, which is environment required `Windows MinGW64` lane
  builds in; there scalar path rounds twice and SIMD path once.
  ADR-1207's gate measured `ssimulacra2` 0.37 points apart on 48-frame clip
  before fix. two call sites today are `picture_to_linear_rgb` in
  `ssimulacra2.c` and both passes of `ms_ssim_decimate.c`. `grep -rn
  '\bfmaf\?('` over scalar feature sources must stay empty for any
  reference with FMA-using twin.
