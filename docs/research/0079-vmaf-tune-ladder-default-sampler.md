<!-- markdownlint-disable MD060 -->
# Research-0079: `vmaf-tune` Phase E ladder default sampler — gap analysis

- **Date**: 2026-05-05
- **Author**: Lusoris
- **Companion ADR**: [ADR-0307](../adr/0307-vmaf-tune-ladder-default-sampler.md)
- **Tags**: tooling, vmaf-tune, ladder, fork-local

## Question

Phase E's `_default_sampler` in
`tools/vmaf-tune/src/vmaftune/ladder.py` raises
`NotImplementedError` with a docstring claiming Phase B's
target-VMAF bisect is "in flight as PR #347". What's the actual
state of that surface, and what's the smallest change that closes
the gap without re-architecting the sampler API?

## Findings

### The Phase B-equivalent predicate already shipped

`tools/vmaf-tune/src/vmaftune/recommend.py::pick_target_vmaf(rows,
target)` returns the smallest-CRF row whose `vmaf_score >= target`,
falling back to the closest-miss row when no row clears the bar
(margin marked `(UNMET)`). The fallback semantics make it safe to
call with any non-empty corpus — there is always a result, even if
the requested target was unreachable.

The canonical row-producing surface is
`corpus.iter_rows(job, opts)`, which already drives the encode +
score loop with subprocess seams (`encode_runner` / `score_runner`)
for tests. Composing the two yields the desired Phase B-equivalent
behaviour: produce N rows, pick the one nearest the target.

### PR #347 was something else

The stale docstring claimed PR #347 was Phase B's bisect. Inspecting
the merged history shows PR #347 actually shipped the
`fr_regressor_v2 codec-aware scaffold` — an unrelated AI-side
addition. The docstring was never refreshed when ADR-0306
(coarse-to-fine search + the `recommend` subcommand) shipped, even
though that PR was effectively the Phase B preview the original Phase
E scaffold gated against.

### What changes if we wire the default

- `_default_sampler` becomes the smallest piece of orchestration that
  composes `iter_rows` with `pick_target_vmaf`.
- `build_ladder()` / `build_and_emit()` no longer raise on the
  documented happy path.
- The `SamplerFn` seam stays open — explicit `sampler=` overrides
  remain the escape hatch for finer grids or non-CRF predicates.
- Tests stub `iter_rows` via `monkeypatch.setattr` so no live encoder
  runs are required. The lazy `from .corpus import iter_rows` inside
  `_default_sampler` resolves through the patched module attribute on
  every call — the test seam is identical to every other
  `tools/vmaf-tune` module's pattern.

### CRF sweep cardinality

The chosen sweep is `(18, 23, 28, 33, 38)` — five points spanning
x264's perceptually-informative range:

- CRF 18 is near-transparent on most content (high VMAF, large
  bitrate).
- CRF 23 is x264's nominal default (medium quality, medium bitrate).
- CRF 28 is the typical midpoint for streaming preview tiers.
- CRF 33 starts visible distortion on demanding content.
- CRF 38 is firmly into "compressed" territory.

Five points matches the cardinality of the ADR-0306 coarse pass
(`coarse_step=10` over `[10..50]` → `(10, 20, 30, 40, 50)`); both
surfaces inherit the same wall-time sizing assumption.

For non-x264 codecs the same five values land inside their nominal
quality ranges (libsvtav1 0..63, libvvenc 0..63, etc.). If the
adapter rejects a candidate via `validate(preset, crf)`, the row's
`exit_status != 0` and the recommend filter drops it — the sampler
gracefully degrades.

## Alternatives considered

| Option | Encodes per cell | Verdict |
|---|---|---|
| **5-point fixed sweep `(18, 23, 28, 33, 38)` (chosen)** | 5 | Mirrors ADR-0306 coarse pass; covers x264's perceptual range; deterministic encode-count for downstream sizing |
| 7-point fixed sweep `(15, 20, 25, 30, 35, 40, 45)` | 7 | 40 % more encode cost; marginal accuracy gain doesn't pay off |
| Adaptive binary bisect over `[0, 51]` | ~6 (variable) | Duplicates `pick_target_vmaf` logic; non-deterministic count makes wall-time sizing harder; struggles with VMAF non-monotonicity at boundary CRFs |

## Decision

Wire `_default_sampler` to compose `iter_rows` + `pick_target_vmaf`
over the 5-point sweep. Keep the `SamplerFn` seam open. Update the
docstring to reflect reality. See ADR-0307 for the full decision +
references.

## References

- Source surface:
  `tools/vmaf-tune/src/vmaftune/ladder.py::_default_sampler`.
- Predicate: `tools/vmaf-tune/src/vmaftune/recommend.py::pick_target_vmaf`.
- Producer: `tools/vmaf-tune/src/vmaftune/corpus.py::iter_rows`.
- Phase E scaffold: [ADR-0295](../adr/0295-vmaf-tune-phase-e-bitrate-ladder.md).
- Phase B-equivalent ship: [ADR-0306](../adr/0306-vmaf-tune-coarse-to-fine.md).
- ADR for this change: [ADR-0307](../adr/0307-vmaf-tune-ladder-default-sampler.md).
