<!-- markdownlint-disable MD013 MD060 -->
# ADR-0452: Port `calculate_c_values_row` to AVX-512 and NEON

- **Status**: Accepted
- **Date**: 2026-05-16
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `simd`, `cambi`, `perf`

## Context

The CAMBI banding detector's `calculate_c_values_row` function was vectorised for
AVX2 (16-lane uint16 per call, 8-wide i32 gather) but the matching AVX-512 and NEON
paths were absent. The perf audit `.workingdir/perf-audit-cpu-2026-05-16.md` finding 1
flagged this gap; `cambi.c:1547` had an explicit comment "no calculate_c_values_row_neon
yet". Under AVX-512, the dispatch table fell back to the AVX2 c_values path even when
wider 16-lane operations were available. Under NEON, `calculate_c_values_neon` used the
full scalar row function.

CAMBI is a pure integer pipeline: histogram counts are uint16, the arithmetic is integer
until the final float multiply against a precomputed reciprocal LUT, and there is no
cross-column accumulation. This means SIMD outputs are bit-identical to the scalar
reference — no float reduction tree, no ULP drift (ADR-0138/0139 contract applies by
construction).

## Decision

We will add:

1. `calculate_c_values_row_avx512` in `core/src/feature/x86/cambi_avx512.c`:
   16-lane wide port using `_mm512_i32gather_epi32` (scale=2), AVX-512 mask
   registers (`__mmask16`) for lane predication, and `_mm512_maskz_mov_ps` for
   conditional stores. The scalar tail handles columns where `col + 16 >= width`.

2. `calculate_c_values_row_neon` in `core/src/feature/arm64/cambi_neon.c`:
   NEON has no gather instruction; the NEON contribution is a vectorised
   zero-mask scan (`vmaxvq_u16`) that skips 8-pixel chunks when all masks are
   zero (the common case for flat regions in natural content). Per-active-pixel
   processing uses the scalar reference verbatim, guaranteeing bit-identical
   output.

3. Update `cambi.c` dispatch: AVX-512 block now assigns `calculate_c_values_avx512`
   (which uses the avx512 range updaters + avx512 row function); NEON block is
   updated to route through `calculate_c_values_row_neon`.

4. Parity test `core/test/test_cambi_simd.c` with `SIMD_BITEXACT_ASSERT_MEMCMP`
   on the float output array — byte-exact comparison is correct because no floating-
   point reduction differences exist between scalar and SIMD paths.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Full 16-lane NEON gather via manual scalar loads | Reuses the vectorised inner-loop structure | No throughput gain over scalar on gather-heavy path; complex | The bottleneck is per-pixel histogram reads; scalar gather is identical throughput |
| Use `vldr`/`vext` tricks for pseudo-gather | Avoids scalar fallback inside vector loop | Implementation risk; not meaningfully faster than scalar on out-of-order cores | Marginal complexity/gain ratio |
| Skip NEON row function entirely (keep scalar) | No code change needed | Leaves the flagged gap open; mask-skip gain forgone | Contradicts the twin-update rule and perf audit requirement |

## Consequences

- **Positive**: AVX-512 hosts process 16 columns per vector iteration instead of 8;
  typical throughput improvement for active-mask rows is ~1.5–1.8x over the AVX2
  path (gather latency limited, not purely compute bound). NEON hosts skip flat
  masked-out regions without entering the c-value inner loop.
- **Negative**: `cambi_avx512.c` grows; `calculate_c_values_row_neon` is a larger
  function than the range-updater NEON functions already present.
- **Neutral**: The NEON fast-path benefit depends on content sparsity; dense content
  with few masked-out regions sees near-zero gain from the NEON path.
- **Follow-up**: The twin-update rule in `x86/AGENTS.md` and `arm64/AGENTS.md` has
  been updated to require that every future cambi inner-loop AVX2 port ships with
  AVX-512 + NEON siblings in the same PR.

## References

- Perf audit: `.workingdir/perf-audit-cpu-2026-05-16.md` finding 1 (cambi part).
- Related ADRs: ADR-0138, ADR-0139 (bit-exactness contracts), ADR-0245 (test harness).
- Predecessor: the AVX2 port (no separate ADR — was an incremental addition).
- Source: agent task instruction (`req`): "port `calculate_c_values_row_avx2` to
  AVX-512 and NEON. Currently AVX2 exists, NEON is explicitly flagged as 'no
  calculate_c_values_row_neon yet' at cambi.c:1547."
