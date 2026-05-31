- CodeQL C bulk sweep: fixed 60 of 78 deferred alerts across 18 upstream-mirror
  files. Categories addressed: `cpp/integer-multiplication-cast-to-long` (44 of
  44 alerts; widening cast prefixed on the LHS operand before multiplication),
  `cpp/commented-out-code` (19 of 19; deleted dead/disabled blocks per the
  no-corpse-comments rule), and `cpp/large-parameter` (1 of 15; converted
  `cambi_score(... CambiBuffers buffers, ...)` to `const CambiBuffers *`). The
  remaining 14 `cpp/large-parameter` alerts on the `VifBuffer` SIMD ABI
  (integer_vif.{c,h}, x86/vif_avx2.c, x86/vif_avx512.c) are deferred to a
  coordinated multi-backend follow-up: changing the VIF calling convention
  ripples to the NEON path (core/src/feature/arm64/vif_neon.{c,h}) which is
  not in this sweep's scope and shares the upstream-mirror pass-by-value
  invariant. Touched files left lint-clean per CLAUDE rule §12 r12; one
  unrelated IWYU warning absorbed in core/test/test_cpu.c (dropped unused
  `config.h` include after deleting the AVX2 mask-test corpse).
