- **`ssimulacra2` scored differently on a host with SIMD than on one
  without.** The four `edge_diff_map` kernels (AVX2, AVX-512, NEON, SVE2)
  computed the per-pixel difference `|img - blur(img)|` with a *float*
  subtract and promoted to double afterwards, while the scalar reference —
  and each kernel's own scalar tail — promote first and subtract in double,
  which is exact for two floats. Because the ssimulacra2 pipeline is
  ill-conditioned downstream (that difference is a catastrophic cancellation
  and pooling takes a 4-norm), the rounding surfaced as a ~1.6e-09 score
  difference between a SIMD host and a scalar one. The per-feature SIMD test
  could not see it: it compares each kernel against a scalar reference defined
  inside the test file, at a 33x21 fixture on which the float subtraction
  happens to be exact. Fixed by folding the subtraction into the per-lane loop
  in double, matching the reference exactly. The fork-added
  `ssimulacra2_test.py` snapshot is unchanged to six decimals, so no snapshot
  was regenerated.

- **New gate: a score may not depend on the host instruction set.**
  `core/test/test_feature_isa_invariance.c` drives the public API twice over
  one fixture — once with the host ISA, once with `cpumask` disabling every
  SIMD flag — and asserts bit-identical scores across ten features. It uses no
  internal symbols, so unlike the per-feature SIMD tests it cannot drift away
  from the shipped code. It found the `edge_diff_map` defect above on its
  first run; the other nine features already passed.
