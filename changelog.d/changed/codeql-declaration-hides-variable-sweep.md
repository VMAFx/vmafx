- chore(libvmaf): rename inner-scope variables that shadowed outer
  scope declarations across `cambi.c` and the AVX2/AVX-512 SIMD paths
  for ADM and VIF. Closes 64 `cpp/declaration-hides-variable` CodeQL
  alerts. Renames are descriptive (semantic role > `_2` suffix); some
  redundant inner redeclarations of identical-typed function-scope
  variables are simply removed. Scope-tightens two j=0 first-column
  blocks in `adm_avx2.c` / `adm_avx512.c` so the per-j tail-loop
  locals no longer shadow the special-case temporaries. Renames in
  the inner `for (fj)` taps of `vif_avx2.c` / `vif_avx512.c`:
  `fq` → `f_tap`, `m0`/`m1` → `m_top`/`m_bot`. Bit-exactness preserved
  (Netflix golden gate green; clang-tidy delta is line-shift only).
