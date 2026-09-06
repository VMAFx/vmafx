<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1208: The ssimulacra2 edge-diff SIMD loops take their difference in double

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: simd, correctness, feature-extractor, reproducibility

## Context

`edge_diff_map` computes, per pixel, `ed = |img - blur(img)|` and then a ratio
of two such quantities. The scalar reference promotes before subtracting:

```c
double ed1 = fabs((double)r1[i] - (double)rm1[i]);
```

Both operands are `float`, so the difference is **exact** in `double`. All four
SIMD kernels instead subtracted in float and promoted afterwards:

```c
const __m512 d1 = |_mm512_sub_ps(a1, am1)|;   double ed1 = (double)d1f[k];
```

which rounds the difference to float first. Each kernel's own scalar tail used
the correct double form, so a single call could mix both conventions depending
on where a pixel fell relative to the vector width.

`test_ssimulacra2_simd::test_edge` compares the SIMD kernel against a scalar
reference defined in the test TU and asserts bit-exactness at 33x21 — and it
passes, because on that fixture's pseudo-random inputs the float subtraction
happens to be exact. On real XYB data it is not. The ADR-1207 ISA-invariance
test found the divergence end-to-end on its first run, and the pipeline's
ill-conditioning (an `|img - blur(img)|` cancellation followed by 4-norm
pooling) turned it into a 1.6e-09 score difference between a SIMD host and a
scalar one — small, but a violation of the fork's bit-exactness contract and of
the property that a score must not depend on the host ISA.

## Decision

We will fold the subtraction into the per-lane loop and take it in `double`,
matching the scalar reference exactly, in all four kernels (AVX2, AVX-512,
NEON, SVE2). The per-lane loop was already scalar — it performs the divide,
the quartic and the accumulation one lane at a time — so the only thing the
vector subtract contributed was the rounding error.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fold the subtraction into the existing scalar per-lane loop (chosen) | Bit-identical to the reference by construction; smallest diff; uniform across all four ISAs | Drops one vector operation from a loop that was already scalar | — |
| Widen to double vectors (`_mm512_cvtps_pd` + `_mm512_sub_pd`) | Keeps the subtraction vectorised and correct | More code and more intrinsic-availability surface per ISA, for a loop whose cost is dominated by the scalar divide and quartic | Rejected — complexity without measurable benefit |
| Change the scalar reference to subtract in float | One-line change | Makes the reference *less* accurate to match an implementation detail, and moves every published score | Rejected — the reference is the contract |
| Accept it and loosen the SIMD test | No code change | Enshrines a host-dependent score, which is the defect | Rejected |

## Consequences

- **Positive**: `ssimulacra2` is now bit-identical with and without SIMD; the
  ADR-1207 gate passes for all ten features with no skips.
- **Negative**: `ssimulacra2` scores from a SIMD host move by ~1e-09 relative to
  before this change (onto the scalar value, which is the reference). The
  fork-added `python/test/ssimulacra2_test.py` snapshot is unaffected —
  measured byte-identical to six decimals on all six assertions — so no
  snapshot was regenerated.
- **Neutral / follow-ups**: `test_ssimulacra2_simd::test_edge` still passes at
  33x21 both before and after, so it remains unable to catch this class on its
  own; ADR-1207's gate is what covers it.

## References

- [ADR-1207](1207-feature-isa-invariance-gate.md) — the test that found this.
- [ADR-0891](0891-simd-bit-exact-round2-fmaf-libvmaf-feature-icx.md) — the
  bit-exactness contract.
- Reproducer: `meson test -C build test_feature_isa_invariance`.
- Source: `req` — user direction to close the recurrence hole in the
  ssimulacra2 SIMD test.
