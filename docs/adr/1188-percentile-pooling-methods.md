# ADR-1188: Percentile temporal pooling in the public C API

- **Status**: Accepted
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: core, api, pooling, abi, output-schema, golden-gate

## Context

`enum VmafPoolingMethod` has exposed only `{UNKNOWN, MIN, MAX, MEAN,
HARMONIC_MEAN}` since libvmaf 2.x, while the Python harness has always offered
`median` / `perc5` / `perc10` / `perc20` by applying `ListStats` to the
per-frame list in NumPy (`compat/python-vmaf/core/result.py`,
`compat/python-vmaf/tools/stats.py`). The consequence is expressive, not
numerical: a C-API, Rust-binding or FFmpeg consumer simply cannot ask for the
"worst 10% of frames" summary that the harness — and Netflix's own
publications — treat as the interesting number for short clips, and
`python/test/quality_runner_test.py:679` pins `perc10 = 72.71845922683059` on
the golden 576x324 pair as proof the Python side computes a real percentile.
Netflix/vmaf#818 reported the gap in 2020; the fork tracked it as
`T-UPSTREAM-818-POOLING-ENUM-NO-PERCENTILES-2026-09-03`.

Two forces constrain the fix. First, `pool_reduce()` is on the golden path:
ADR-1118 requires the unweighted `MEAN` / `HARMONIC_MEAN` arithmetic to stay
byte-identical to upstream, so percentile support must not perturb the
accumulator loop. Second, `pooled_metrics` in the XML / JSON logs is a consumed
output schema, and the writers used to enumerate `[1, VMAF_POOL_METHOD_NB)`, so
appending enumerators would silently widen every log file the fork emits.

## Decision

We append `VMAF_POOL_METHOD_MEDIAN`, `_PERC5`, `_PERC10` and `_PERC20` after
`HARMONIC_MEAN` (append-only, so every existing discriminant keeps its value),
compute them by retaining the per-frame scores in a geometrically-grown buffer
that is allocated *only* for those four methods, and reduce them with the
`numpy.percentile(method="linear")` rule already used for the bootstrap
confidence intervals — now shared as `static inline` helpers in
`core/src/percentile.h`. Percentiles are pure order statistics and therefore
ignore ADR-1118 perceptual weighting, exactly as `MIN` and `MAX` do. The log
writers keep emitting the historical four methods via an explicit
`pool_report_order[]` table instead of iterating to `VMAF_POOL_METHOD_NB`, so
the `pooled_metrics` schema is unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Append the four enumerators; buffer per-frame scores only for them (**chosen**) | ABI-safe; accumulator path untouched, so the golden gate cannot move; one percentile rule shared with the bootstrap CIs | Percentile pools are O(n) memory and O(n log n) time | — |
| Always collect the per-frame vector and derive every method from it | One code path; min/max/mean fall out of the sorted vector | Changes the summation order of `MEAN` / `HARMONIC_MEAN`, i.e. moves golden numbers; unbounded allocation for `index_high == UINT_MAX` | Violates ADR-1118 golden-gate isolation |
| Weight percentiles with the ADR-1118 perceptual weights | Consistent with `MEAN` when weighting is on | Weighted order statistics need a weighted-rank definition that NumPy's `ListStats` does not use, so C and Python would disagree — the exact defect being closed | Rejected; documented in the header instead |
| Also emit `median` / `perc*` in `pooled_metrics` | Percentiles visible from the CLI with no new flag | Widens a consumed output schema (XML attributes, JSON keys, harness parsers) as a side effect of an enum append | Deferred; needs its own opt-in surface |
| Leave the gap; tell callers to pool in their own code | Zero risk | Every consumer re-implements interpolation and drifts from the harness; keeps a five-year-old report open | Rejected |

## Consequences

- **Positive**: C, Rust and FFmpeg consumers can request `median`, `perc5`,
  `perc10`, `perc20`; the C result matches the Python harness bit-for-bit for
  the same per-frame vector, because both interpolate linearly between ranks.
- **Negative**: a percentile pool holds `8 × n_frames` bytes and sorts, so it
  is no longer O(1) space like the accumulator methods. `VMAF_POOL_METHOD_NB`
  moves from 5 to 9 — it is already documented as an unstable count sentinel,
  and `-DVMAF_BUILDING_LIBVMAF` keeps internal iteration warning-free.
- **Neutral / follow-ups**: `pooled_metrics` output is deliberately unchanged;
  exposing percentiles through a CLI flag or a widened log schema is a separate
  decision. `ffmpeg-patches/0018` maps the new option strings for the FFmpeg
  filters.

## References

- Netflix/vmaf#818 — "pooling method enum has no percentiles".
- Ledger row `T-UPSTREAM-818-POOLING-ENUM-NO-PERCENTILES-2026-09-03` in
  [`docs/state.md`](../state.md).
- [ADR-1118](1118-perceptual-sidedata-weighting.md) — golden-gate
  isolation for the pooling accumulators.
- [ADR-0165](0165-state-md-bug-tracking.md) — ledger discipline.
- `python/test/quality_runner_test.py:679` — golden `perc10` assertion.
- Source: agent task brief for
  `T-UPSTREAM-818-POOLING-ENUM-NO-PERCENTILES-2026-09-03` (paraphrased: close
  the verified pooling-enum gap, append-only and ABI-safe, reusing the existing
  percentile helper so C and the Python harness agree).
