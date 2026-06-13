<!-- markdownlint-disable MD060 -->
# Research-0090: vmaf-tune Phase B target-VMAF bisect — feasibility

- **Date**: 2026-05-08
- **Author**: lusoris
- **Companion ADR**: [ADR-0326](../adr/0326-vmaf-tune-phase-b-bisect.md)

## Question

What is the right algorithmic primitive for Phase B (target-VMAF
bisect) of `vmaf-tune`? How many real encodes does it cost per call,
and how does it compose with the existing coarse-to-fine grid search
([ADR-0306](../adr/0306-vmaf-tune-coarse-to-fine.md))?

## Algorithm choice

Three candidates, evaluated against integer-CRF, monotone-VMAF-in-CRF
real-encode constraints:

| Algorithm | Encodes per call | Convergence | Notes |
|---|---|---|---|
| Integer binary search (chosen) | `ceil(log2(range)) ≈ 6` for `range=51` | Tight `O(log)` | Mirrors `predictor.pick_crf`; clean monotonicity assertion |
| Golden-section search | ~`log_φ(range) ≈ 8.4` for `range=51` | Same asymptote, irregular below 4-CRF windows | Continuous-domain assumption fights integer rounding |
| Brute-force linear scan | `range = 52` for x264 | `O(n)` | ~9× more encodes; ignores monotonicity instead of asserting it |
| Bayesian (Optuna TPE) | 10–20 trials typical | Robust to non-monotone curves | Pulls Optuna into the production predicate; the `fast` subcommand already owns this surface |

Binary search wins on every axis except "robustness to non-monotone
curves", and for that case we deliberately abort rather than degrade
into a slower fallback (the AGENTS.md invariant). Real-world content +
modern codecs are monotone in CRF; pathological exceptions are
encoder bugs we want to surface, not paper over.

## Expected encode budget per call

The codec adapter's `quality_range` for x264 widens to `(0, 51)` per
[ADR-0306](../adr/0306-vmaf-tune-coarse-to-fine.md), so the worst-case
binary search converges in `ceil(log2(52)) = 6` halvings. We cap at
`max_iterations=8` to absorb the off-by-one between window collapse
and the "best so far" promotion path. With a 2-to-3-second per-encode
wall time at 1080p `medium` preset on a single CPU socket, one bisect
call costs **12–24 wall-clock seconds**. Phase E ladder generation
runs the bisect five times per resolution per target VMAF, so a
5-resolution × 3-target-VMAF ladder is **~3–6 wall-clock minutes**
on CPU; per [ADR-0299](../adr/0299-vmaf-tune-gpu-score.md) the score
side is GPU-accelerated, halving that further.

## Composition with coarse-to-fine

[ADR-0306](../adr/0306-vmaf-tune-coarse-to-fine.md) ships
`corpus.coarse_to_fine_search` as a 2-pass `(preset × CRF)` grid
search. Phase B is the **single-axis primitive** the coarse-to-fine
loop wraps when only the CRF axis is in play:

- Coarse-to-fine sweeps a `presets × CRF-coarse` grid first (~15 cells
  for x264), picks the best preset, then runs a `CRF-fine` pass at
  that preset (~5 cells). Total: ~20 encodes.
- Phase B is what `recommend.pick_target_vmaf` could use as a callback
  if the operator pinned a preset up-front and only needed CRF
  optimised. Total: ~6 encodes.

The two are complementary, not redundant. Coarse-to-fine spans a
2-axis space; bisect spans a 1-axis space inside that. We do **not**
fold coarse-to-fine into the bisect entry-point — the API surface
stays minimal so callers compose them explicitly.

## Recommendation

Ship the integer binary search per the ADR. Resist the temptation to
add fallback strategies for monotonicity-violation cases; surfacing
the violation is more useful than recovering from it.

## References

- [ADR-0326](../adr/0326-vmaf-tune-phase-b-bisect.md) — the decision.
- [`predictor.pick_crf`](../../tools/vmaf-tune/src/vmaftune/predictor.py)
  — the analytical-curve precedent for the same algorithmic shape.
- [ADR-0306](../adr/0306-vmaf-tune-coarse-to-fine.md) — coarse-to-fine
  grid search; complementary primitive.
- [ADR-0237](../adr/0237-quality-aware-encode-automation.md) — vmaf-tune
  umbrella spec.
