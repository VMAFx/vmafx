- **float-ADM NEON DWT2 is now bit-exact with the scalar reference on
  aarch64**: completes the ADR-1057 follow-up. The dispatched NEON DWT2
  kernel (`float_adm_dwt2_neon`) is FMA-free and compiled `-ffp-contract=off`,
  but on aarch64 the scalar reference `adm_dwt2_s`/`adm_dwt2_lo_s` were being
  FMA-contracted by the compiler default (`-ffp-contract=fast` on GCC,
  `on` on Clang — both fuse `accum += a*b` into a single-rounded `fmadd`),
  so the runtime scalar-fallback path diverged from the NEON path by ~1 ULP.
  (This was the actual root cause behind the PR #685 -> #695 revert; the
  divergence was the *scalar* side fusing, not only the NEON intrinsics.)
  Fix: `adm_tools.c` now includes `config.h` (the prior `#ifdef HAVE_CONFIG_H`
  guard was dead, so `ARCH_AARCH64` never reached the TU), and the two scalar
  single-precision DWT2 functions carry an aarch64-only `-ffp-contract=off`
  guard (`#pragma clang fp contract(off)` for Clang, the `optimize` function
  attribute for GCC). x86 is byte-for-byte unaffected — `ARCH_AARCH64` is
  undefined there and GCC already defaults to `-ffp-contract=off` on x86 — so
  the Netflix golden gate is untouched. A new load-bearing parity test,
  `test_float_adm_simd` (`test_float_adm_dwt2_bitexact`-equivalent), asserts
  `float_adm_dwt2_neon == adm_dwt2_s` via `memcmp` across nine fixtures and is
  registered in the `fast`/`simd` suites. Verified bit-exact on GCC 16.1 and
  Clang 22 under `qemu-aarch64` across all nine cases (the original kernel
  diverged on all nine — negative control confirmed). (ADR-1057)
