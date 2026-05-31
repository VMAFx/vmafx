<!-- markdownlint-disable MD060 -->
# ADR-0502: ADM decouple gather prefetch (Approach B)

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `simd`, `perf`, `adm`, `avx512`, `fork-local`

## Context

The `adm_decouple_avx512` function in
`core/src/feature/x86/adm_avx512.c` spends 66.5 % of its own
cycles — 2.31 % of total VMAF wall time — on three `vpgatherdd`
instructions that look up the 256 KB `adm_div_lookup` table
(65 537 × int32, indexed by `band_h/v/d + 32768`).

The LUT exceeds L1 capacity (32–48 KB) and saturates L2 (256–512 KB)
because DWT sub-band coefficients are drawn arbitrarily from
[−32 768, +32 767] with no monotone ordering within a row.  Each
gather therefore issues up to 48 scatter reads with frequent L2/L3
misses, stalling the execution port that feeds the downstream
arithmetic.

The perf-profiler identified two candidate approaches:

- **Approach A** — replace gather with `vpermd` + sequential loads if
  indices are monotone per row.
- **Approach B** — issue software prefetches 2 iterations ahead if
  indices are not monotone.

On inspecting the index-compute path (`oh`, `ov`, `od` are raw DWT
coefficients sign-extended from `int16_t`; their values are
input-dependent and have no ordering guarantee) Approach A is not
applicable.  Approach B is implemented here.

## Decision

We will prefetch the `adm_div_lookup` cache lines corresponding to
the iteration 2 steps ahead (`j + 32` elements) into L2 using
`_MM_HINT_T1` before each set of three `vpgatherdd` instructions.
The prefetch loop reads 16 int16 values per band from the
next-next-iteration pointers, computes `[val + 32768]` offsets, and
calls `_mm_prefetch`.  No computed value changes; the patch is a
pure access-strategy change.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Approach A — vpermd + sequential load** | Eliminates gather latency entirely; best throughput | Requires monotone-increasing indices within a row | DWT coefficients are arbitrary int16; monotone ordering does not hold |
| **Approach B — software prefetch (chosen)** | Hides L2/L3 miss latency; preserves gather semantics; zero risk to bit-exactness | Adds 48 scalar prefetch instructions per iteration; guard branch needed for row-end | Only option given non-monotone indices |
| **No change** | Zero risk | 2.31 % of wall time left on table | Does not meet the T4 perf-win bar |

## Consequences

- **Positive**: −5.8 % total wall time on BBB 1080p (302-frame, 8-run
  mean: 9 603 ms → 9 049 ms); gathered miss latency hidden behind
  ~300 arithmetic cycles.
- **Negative**: +48 `prefetchT1` instructions per inner-loop
  iteration; negligible on modern front-ends (prefetch is a hint,
  does not stall).
- **Neutral / follow-ups**: The scalar tail loop (`j = right_mod16 …
  right`) does not use the LUT in a vectorized manner and is
  unmodified.  The `adm_decouple_avx2` path is not affected; it uses
  `_mm256_i32gather_epi32` on the same LUT but that function is not
  in the hot path at this profile depth.

## References

- perf-profiler finding: `adm_decouple_avx512` gather cluster = 66.5 %
  of function cycles = 2.31 % total wall time (Perf Win #4).
- [Research-0435](../research/0435-adm-decouple-gather-locality.md)
- Related: [ADR-0500](0500-vif-perf-lut-shrink-and-filter-cache.md) —
  VIF LUT shrink (similar gather-miss problem, different solution space).
- PR: `perf/adm-decouple-gather-locality-2026-05-18`
