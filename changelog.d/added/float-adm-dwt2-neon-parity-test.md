- **NEON parity test for the float-ADM DWT2 kernel
  (`core/test/test_float_adm_dwt2_neon.c`).** `float_adm_dwt2_neon` had no
  unit test on any architecture — the same coverage hole that let ADR-1057's
  dropped filter tap reach master in the integer twin. The new test compares
  the NEON kernel against the *production* scalar `adm_dwt2_s()` (linked out
  of `libvmaf_feature_static_lib`, not transcribed, so the ADR-1057
  FP-contraction contract is part of what is under test) and requires
  bit-exact equality of all four subbands, plus proof that nothing is written
  into the destination stride padding.

  Coverage: 29 hand-picked geometries — every `w % 4` residue, widths below
  the 4-column vector stride, both height parities, independently padded
  source and destination strides, and the full ADM scale pyramid for the
  576x324 Netflix golden clip (576x324 → 288x162 → 144x81 → 72x41 → 36x21) —
  plus an exhaustive sweep of `w` in 2..40 x `h` in 2..12. `adm_dwt2_dispatch()`
  in `adm.c` applies no width guard, so every one of those widths is a width
  the production path really takes.

  The kernel passed on first run: no divergence found, and no production
  source was changed. An A/B of the 576x324 golden pair with NEON enabled
  versus `--cpumask 4294967295` (all SIMD off) is byte-identical at `%.17g`
  precision across all 20 frames and the pooled metrics. Registered in suite
  `['fast', 'simd']`, gated on `float_enabled` and aarch64.
