- **SpEED AVX2 matrix product is bit-exact on the Intel compiler too.** The
  kernel's vector bodies pin their rounding with separate `_mm256_mul_ps` /
  `_mm256_add_ps` intrinsics, but its scalar tail was plain C (`acc += a * b`)
  and relied on the translation unit's `-ffp-contract=off`. That flag does not
  survive its own command line under icx: `-fp-model=precise`, added to the
  same carve-out to *strengthen* strict FP, implies `-ffp-contract=on` and lands
  after it, so contraction was enabled. The tail fused into a `VFMADD`, rounded
  once instead of twice, and diverged from `speed_matmul_scalar` at column 24 of
  SpEED's 25-wide QR shape — the `Linux Intel LLVM` lane's
  `test_speed_simd` failure. The tail is now `_mm_mul_ss` / `_mm_add_ss`, which
  are not routed through `llvm.fmuladd` and so pin the rounding regardless of
  compiler or flag order. The wider flag-ordering defect is recorded as
  `T-ICX-FP-CONTRACT-FLAG-ORDER-2026-09-07` in `docs/state.md`.
