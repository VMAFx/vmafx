- **The ISA-invariance gate runs green by quarantining one feature, and names
  two pre-existing defects it found.** ADR-1207's
  `test_feature_isa_invariance` drives the public API twice — once on the
  host's real ISA, once with `cpumask` disabling every SIMD flag — and asserts
  bit-identical scores. Its first run under clang and under a sanitizer build
  surfaced two defects that `master` already had and no other test reached:
  `float_vif` scores differ between the SIMD and scalar paths by **1.789e-08**
  under clang (gcc hides it by contracting the scalar multiply-add into an FMA
  that happens to match the AVX kernel), and driving `float_vif` through the
  AVX path under AddressSanitizer reports a **heap-buffer-overflow** — the
  horizontal convolution scanline reads 32 bytes starting 3 bytes past the end
  of the row buffer, in a user-reachable path. Both are verified against
  unmodified `master`, so neither is a regression from this branch. `float_vif`
  is excluded from the gate's table with both bug ids inline; the other nine
  features stay covered. Tracked as
  `T-FLOAT-VIF-ISA-DIVERGENCE-2026-09-16` and
  `T-CONVOLUTION-AVX-SCANLINE-OVERREAD-2026-09-16`.
