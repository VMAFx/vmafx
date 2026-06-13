# ADR-0179: float_moment SIMD parity (AVX2 + NEON)

- **Status**: Accepted
- **Date**: 2026-04-26
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: simd, x86, arm64, feature-extractor, fork-local

## Context

The fork's SIMD coverage matrix
([`.workingdir2/analysis/metrics-backends-matrix.md`](../../.workingdir2/analysis/metrics-backends-matrix.md))
flagged `float_moment` as the only fully-scalar row remaining after
T5-1c closed. The extractor produces four scores
(`float_moment_{ref,dis}{1st,2nd}`) via two trivial reductions over
the picture: a sum and a sum-of-squares. Every other float-domain
extractor in the fork (ANSNR, MS-SSIM decimate, PSNR-HVS, MOTION,
SSIM, SSIMULACRA 2) already has at least AVX2 + NEON paths.
Closing the row removes the last scalar bottleneck on the float
side of the matrix and keeps the SIMD-coverage backlog (T7-19..T7-25)
moving in priority order.

## Decision

We add AVX2 (8-wide) and NEON (4-wide) implementations of
`compute_1st_moment` / `compute_2nd_moment` under
`core/src/feature/{x86,arm64}/moment_*.{c,h}` and dispatch to
them from `float_moment.c` via function pointers selected at
`init()` from `vmaf_get_cpu_flags()`. Both paths follow the
`ansnr_avx2.c` pattern: square in float (matching the scalar
reference), accumulate into `double` either via a scattered tmp
buffer (AVX2) or via lane-pair widening (`vcvt_f64_f32`, NEON).
The contract is **tolerance-bounded**, not bit-exact, with the
residual confirmed below `1e-7` relative on representative inputs
— five orders of magnitude tighter than the production snapshot
gate's `places=4`. Tests: a new `test_moment_simd` runs four
cases per arch (two random seeds, an aligned width, and a tiny
edge case to exercise the per-row tail).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Bit-exact lane-by-lane reduction (mirror scalar order verbatim) | Byte-equal output | Ignores the established fork pattern (ANSNR, MOTION, MS-SSIM all already accept tolerance); kernel author flagged in `moment_avx2.c` header that the residual is "well inside the snapshot gate's tolerance" | Pattern parity wins — bit-exactness is unnecessary at `places=4` and forecloses obvious wins (lane reductions, FMA on AArch64) |
| AVX-512 path now | Future-proof on Sapphire Rapids / Zen 5 | The 1st/2nd moment are memory-bound on a 4096-pixel reduction; AVX-512 wouldn't deliver a measurable win over AVX2 here | Deferred to T7-21 if profiling motivates it — for now the AVX2 path closes the matrix gap |
| Skip the gap-fill — moment is rarely the hot path | Less code | The matrix audit goal is *no scalar rows*, both for correctness signal (a SIMD path catches new compiler regressions in the scalar) and for snapshot stability across builds | Gap-fill is part of the audit programme — leaving the row scalar contradicts the matrix's purpose |

## Consequences

- **Positive**: SIMD-coverage matrix now has zero fully-scalar
  float rows. New `test_moment_simd` exercises both AVX2 and
  NEON paths against the scalar reference on every arch-matching
  CI lane. End-to-end CLI output unchanged at JSON `%g`
  precision (verified locally — diff between AVX2 dispatcher
  and `--cpumask` scalar-fallback rounds to zero).
- **Negative**: One more dispatch site to maintain when CPU-flag
  wiring changes; one more pair of files to keep in sync with
  the scalar reference if Netflix ever updates `moment.c`.
- **Neutral / follow-ups**: T7-21 (`psnr_hvs` AVX-512 audit) and
  T7-20 (`integer_ansnr` SIMD parity) remain the next gaps in
  priority order. No model retraining required — the extractor's
  numerical contract is unchanged.

## References

- Source: per `.workingdir2/analysis/metrics-backends-matrix.md`
  ordering and user direction on 2026-04-26 ("start filling the
  gaps of metrics-backends-matrix.md (meaning implementing the
  gaps)").
- Backlog row: `T7-19` in
  [`.workingdir2/BACKLOG.md`](../../.workingdir2/BACKLOG.md).
- Pattern parent: [ADR-0125](0125-ms-ssim-decimate-simd.md)
  (MS-SSIM decimate SIMD), [ADR-0159](0159-psnr-hvs-avx2-bitexact.md)
  (PSNR-HVS AVX2), [ADR-0161](0161-ssimulacra2-simd-bitexact.md)
  (SSIMULACRA 2 SIMD).
- Related: `req` — see commit footnote and PR description.
