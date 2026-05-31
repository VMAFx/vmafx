<!-- markdownlint-disable MD060 -->
# ADR-0307: `vmaf-tune` ladder default sampler — wire Phase B/E gap

- **Status**: Accepted
- **Date**: 2026-05-05
- **Deciders**: Lusoris
- **Tags**: tooling, automation, vmaf-tune, ladder, fork-local

## Context

Phase E (`tools/vmaf-tune/src/vmaftune/ladder.py`) shipped as a
sampler-pluggable scaffold under
[ADR-0295](0295-vmaf-tune-phase-e-bitrate-ladder.md) with a
default `_default_sampler` that raised `NotImplementedError`. The
guard's docstring claimed Phase B's target-VMAF bisect (PR #347) was
"in flight" and that the default would be wired once that landed —
but PR #347 actually shipped the `fr_regressor_v2` codec-aware
scaffold (an unrelated AI-side surface), and the comment never got
updated.

Meanwhile, the functional equivalent of Phase B's predicate already
exists on master:
`tools/vmaf-tune/src/vmaftune/recommend.py::pick_target_vmaf` returns
the smallest-CRF row whose VMAF clears a target (falling back to the
closest-miss row when none clears) over a corpus produced by
`corpus.iter_rows`. That is exactly the seam Phase E's default sampler
needs — the missing wiring is mechanical, not architectural.

The status quo (raising `NotImplementedError`) forces every
`vmaf-tune` Phase E caller — including downstream tooling and any
internal smoke run — to supply a sampler argument by hand or fail.
Closing the gap unblocks the documented `build_ladder()` /
`build_and_emit()` happy paths without changing any contract: the
`SamplerFn` seam stays open for callers needing a finer grid or a
non-CRF-based search (Bayesian, GP, precomputed corpus stream).

## Decision

`_default_sampler` will compose `corpus.iter_rows` with
`recommend.pick_target_vmaf` over a fixed 5-point CRF sweep
`DEFAULT_SAMPLER_CRF_SWEEP = (18, 23, 28, 33, 38)` at the codec
adapter's mid-range preset (`"medium"` for libx264 / libx265 /
libsvtav1; otherwise the midpoint of the adapter's `presets` tuple).
The corpus is written to a `tempfile.TemporaryDirectory`-scoped JSONL
that is discarded after the sampler returns; encoded outputs land in
the same temp tree so cleanup is automatic.

The 5-point sweep covers the perceptually-informative range for x264
(CRF 18 is near-transparent on most content; CRF 38 is firmly
distorted) at the same number of probes as the canonical CRF coarse
pass in [ADR-0306](0306-vmaf-tune-coarse-to-fine.md). Five encodes is
the wall-time budget Phase E's downstream sizing already assumes per
`(resolution, target_vmaf)` cell.

The `SamplerFn` seam stays open. Callers needing a finer grid, a
Bayesian bisect, or a precomputed corpus stream pass an explicit
`sampler=` to `build_ladder()` / `build_and_emit()`. Tests stub
`iter_rows` via
`monkeypatch.setattr(corpus_module, "iter_rows", ...)`; the lazy
`from .corpus import iter_rows` inside `_default_sampler` resolves
through the patched module attribute on every call.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| 5-point fixed sweep `(18, 23, 28, 33, 38)` (chosen) | Mirrors ADR-0306 coarse-pass cardinality; covers x264's perceptually-informative range; deterministic encode count for downstream wall-time sizing; trivial to reason about | Coarser than a binary bisect at the cost of one extra encode per cell when target VMAF lands between probes | Best balance of simplicity, predictability, and coverage; Phase E callers can always pass `sampler=` for a finer grid |
| 7-point fixed sweep `(15, 20, 25, 30, 35, 40, 45)` | Tighter CRF resolution — closer match to target | 40 % more encodes per cell; wall-time impact compounds across (resolution × target_vmaf) grid | Phase E's wall-time budget is already the dominant cost; the marginal accuracy gain is dwarfed by the encode-time hit |
| Adaptive bisect (binary search over CRF) | Optimal probe count asymptotically (~log₂(51) ≈ 6 encodes max) | Duplicates `recommend.pick_target_vmaf`'s existing logic; non-deterministic encode count makes wall-time sizing harder; struggles with VMAF non-monotonicity at boundary CRFs | Existing `pick_target_vmaf` already does the picking; adding a parallel adaptive search adds maintenance debt without buying clarity over the fixed sweep |

## Consequences

- **Positive**:
  - `build_ladder()` and `build_and_emit()` no longer raise on the
    documented happy path — the docstring promise is now real.
  - Composition over duplication: every encode + score still flows
    through `corpus.iter_rows`, every pick still flows through
    `recommend.pick_target_vmaf`. No new orchestration path.
  - `SamplerFn` seam stays open — production tooling that needs
    finer control passes `sampler=` and gets it.
- **Negative**:
  - The 5-point default does not adapt to source difficulty —
    pathological clips may want a finer grid. Mitigated by the
    explicit `sampler=` override.
  - The default sampler treats the source as a raw YUV at
    `yuv420p`/24 fps with 1-second nominal duration; non-default
    framerates / pix_fmts must use an explicit sampler. ADR-0307
    keeps this minimal because the docstring already names
    `sampler=` as the override seam.
- **Neutral / follow-ups**:
  - Phase B (proper target-VMAF bisect with confidence intervals)
    can still ship as an upgrade — the seam is intact.
  - Phase E end-to-end smoke against real ffmpeg + libvmaf binaries
    is left to a follow-up; this PR is wiring + unit-stubbed tests.

## References

- Parent: [ADR-0237](0237-quality-aware-encode-automation.md) —
  quality-aware encode automation roadmap.
- Phase E scaffold: [ADR-0295](0295-vmaf-tune-phase-e-bitrate-ladder.md).
- Predicate source:
  [ADR-0306](0306-vmaf-tune-coarse-to-fine.md) — coarse-to-fine search
  surfaces `pick_target_vmaf` as the canonical Phase B-equivalent
  predicate.
- Companion research digest:
  [`docs/research/0079-vmaf-tune-ladder-default-sampler.md`](../research/0079-vmaf-tune-ladder-default-sampler.md).
- Source: `req` — paraphrased: close the Phase B/E wiring gap; the
  `_default_sampler` raise is stale because `recommend.pick_target_vmaf`
  already provides the predicate; the 5-point CRF sweep is the
  deterministic-encode-count default; explicit `sampler=` remains
  supported for finer grids.
