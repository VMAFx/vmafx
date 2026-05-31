<!-- markdownlint-disable MD060 -->
# ADR-0326: vmaf-tune Phase B — target-VMAF bisect

- **Status**: Accepted
- **Status update 2026-05-15**: implemented;
  `tools/vmaf-tune/src/vmaftune/bisect.py` present on master;
  `BisectResult` + `bisect_target_vmaf()` exported; Phase B released.
- **Date**: 2026-05-08
- **Deciders**: lusoris
- **Tags**: tooling, vmaf-tune, fork-local

## Context

The `vmaf-tune` harness has shipped Phase A (corpus grid sweep), Phase A.5
(`fast` Optuna proxy), Phase C (`predict`, [ADR-0276](0276-vmaf-tune-fast-path.md)),
Phase D (`tune-per-shot`), Phase E (`ladder`,
[ADR-0295](0295-vmaf-tune-phase-e-bitrate-ladder.md)), plus the `compare`
and `recommend-saliency` subcommands. Each of these surfaces depends on
a **target-VMAF predicate** with the loose contract "given a source +
codec + quality floor, return the most-compressed encode that still
clears the floor". Until this PR the production predicate raised
`NotImplementedError("Phase B pending")`; tests injected a fake
predicate, production callers had no working backend.

The "Phase B pending" placeholder has been referenced from
[ADR-0276](0276-vmaf-tune-fast-path.md),
[ADR-0287](0287-vmaf-tune-saliency-aware-encoding.md),
[ADR-0295](0295-vmaf-tune-phase-e-bitrate-ladder.md),
[ADR-0306](0306-vmaf-tune-coarse-to-fine.md), and others as the
"production wiring" they each defer to. Promoting the placeholder is
overdue.

The analytical-curve binary search in
[`predictor.pick_crf`](../../tools/vmaf-tune/src/vmaftune/predictor.py)
already establishes the algorithmic shape on synthetic data; Phase B
mirrors it onto real encodes via the existing
[`encode.run_encode`](../../tools/vmaf-tune/src/vmaftune/encode.py) /
[`score.run_score`](../../tools/vmaf-tune/src/vmaftune/score.py)
subprocess seams. The codec-adapter registry already exposes the
search-space boundary (`adapter.quality_range`, per
[ADR-0296](0296-vmaf-roi-saliency-weighted.md)), so the bisect picks
that as the default search domain. A separate coarse-to-fine search
([ADR-0306](0306-vmaf-tune-coarse-to-fine.md)) lives in
`corpus.coarse_to_fine_search` for the broader (preset, CRF) grid; the
bisect is the single-axis primitive coarse-to-fine wraps when only the
CRF axis is in play.

## Decision

We will ship `vmaftune.bisect` as a pure module with three exported
symbols:

1. `BisectResult` — `(codec, best_crf, measured_vmaf, bitrate_kbps,
   encode_time_ms, n_iterations, encoder_version, ok, error)`. Mirrors
   the shape of `compare.RecommendResult` so a one-line adapter
   (`make_bisect_predicate`) satisfies the existing `compare.PredicateFn`
   signature.
2. `bisect_target_vmaf(src, codec, target_vmaf, *, width, height, ...,
   crf_range=None, max_iterations=8, encode_runner=None,
   score_runner=None, ...) -> BisectResult` — the core algorithm.
3. `make_bisect_predicate(target_vmaf, *, width, height, ...) ->
   compare.PredicateFn` — the closure-binding adapter.

The algorithm is a textbook integer binary search over CRF assuming
monotone-decreasing VMAF in CRF:

```text
lo, hi = crf_range or adapter.quality_range
while lo <= hi and n_iterations < max_iterations:
    mid = midpoint_lower_quality(lo, hi)        # round toward higher CRF
    measured = score(encode(src, codec, mid))
    if measured >= target_vmaf:
        best = (mid, measured); lo = mid + 1     # try harder compression
    else:
        hi = mid - 1                              # need higher quality
```

The midpoint rounds toward the lower-quality (higher-CRF) end of the
window so the "best so far" we accept is always a CRF we actually
measured — never one extrapolated to from an adjacent sample. This is
a one-line correctness guard, not a performance choice.

The bisect aborts with a clear error when:

- The target is unreachable in the searched window (target above the
  curve's maximum) — `ok=False, error="target ... unreachable in CRF
  window ..."` and the closest-miss CRF is reported in the error
  string.
- Two non-adjacent samples violate monotone-decreasing VMAF in CRF by
  more than 0.5 VMAF (looser than measurement noise on a single
  shot) — `ok=False, error="monotonicity violation ..."`. The bisect
  never falls back to a non-bisect strategy in this case; the
  `tools/vmaf-tune/AGENTS.md` invariant is "monotonicity is a hard
  contract; bail with a clear error if it doesn't hold".

The module is **subprocess-free in tests**: `encode_runner` /
`score_runner` mirror the pattern from `encode.run_encode` /
`score.run_score`, and the test suite exercises the full bisect
(including the predicate adapter and `compare_codecs` integration)
with synthetic curves.

`compare._default_predicate` is updated to point callers at
`bisect.make_bisect_predicate` rather than name "Phase B pending"; the
predicate signature `(codec, src, target_vmaf) -> RecommendResult` does
not carry source geometry so operators bind geometry once via
`make_bisect_predicate(width=..., height=..., framerate=...,
duration_s=...)` and pass the closure into `compare_codecs(predicate=...)`
or via `--predicate-module MODULE:CALLABLE`. The default predicate
stays a pointer rather than the production wiring because making
`compare`'s top-level signature carry geometry would break the existing
ranking contract for every other caller.

No new CLI subcommand. The bisect is a programmatic primitive that the
existing `compare` / `recommend-saliency` / `predict` / `tune-per-shot`
/ `ladder` subcommands consume via the predicate seam.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Binary search (chosen)** | `O(log range)` encodes; mirrors the proven `predictor.pick_crf` shape; trivial to reason about | Assumes monotone-decreasing VMAF in CRF (real-world content satisfies this for every modern codec; we hard-bail when it doesn't) | Clear winner — the assumption is sound and the algorithm is the smallest correct primitive |
| Golden-section search | Optimal for unimodal continuous functions; one fewer evaluation per halving | CRF is integer-valued; golden-section's `(φ, 1/φ)` partition does not respect integer steps; convergence becomes irregular below 4-CRF windows | Cost outweighs the saving once we cap at 8 iterations |
| Full coarse-to-fine grid (ADR-0306) | Explores the (preset, CRF) plane; robust against pathological curves | Encodes the entire grid; ~15–25 encodes per call vs the bisect's 6–8; over-budget for any per-shot or per-resolution outer loop | Already shipped (`corpus.coarse_to_fine_search`) for the use case it fits — Phase B is the single-axis inner loop, not a replacement |
| Brute-force linear scan | Trivially correct; catches non-monotone curves | `O(range)` encodes — `(0..51)` is 52 encodes per call; inflates Phase E ladder generation by ~50× | Dismissed; same monotonicity assumption applies, and brute force ignores it instead of asserting it |
| Bayesian optimisation (e.g. Optuna `TPESampler`) | Handles non-monotone curves; the `fast` subcommand already wires Optuna | Optuna is an opt-in dep (ADR-0276); pulling it into the production predicate breaks the zero-dep corpus path | The `fast` subcommand owns this niche; Phase B stays pure stdlib |

## Consequences

- **Positive**: Every existing subcommand that had a stubbed predicate
  has a real production wiring with one closure-binding step. The
  monotonicity invariant is enforced rather than assumed silently.
  The 6–8-encode budget per call lets Phase E generate a five-tier
  per-resolution ladder in well under a wall-clock minute on modest
  hardware once GPU-side encode is wired in.
- **Negative**: The predicate signature in `compare.PredicateFn`
  cannot carry source geometry, so the default predicate still
  surfaces an error when called with no closure binding. This is a
  documented one-line hand-shake, not a regression — pre-Phase-B the
  default predicate raised `NotImplementedError`.
- **Neutral / follow-ups**:
  - Sample-clip mode ([ADR-0301](0301-vmaf-tune-sample-clip.md)) is
    out of scope for the first cut; the bisect always encodes the
    full source. Wiring `sample_clip_seconds` through is a small
    follow-up that mirrors `corpus._resolve_sample_clip`.
  - Cache integration ([ADR-0298](0298-vmaf-tune-cache.md)) is not yet
    wired; the bisect re-encodes on every call. The cache key fields
    are already adapter-aware so the wiring is a one-call insertion
    in `_encode_and_score`.
  - The bisect module is a candidate for the future `vmaf-tune
    bisect` standalone CLI subcommand if operator demand surfaces;
    today's wiring keeps it a programmatic primitive only.

## References

- [ADR-0237](0237-quality-aware-encode-automation.md) — vmaf-tune
  umbrella spec.
- [ADR-0276](0276-vmaf-tune-fast-path.md) — Phase A.5 fast path; cites
  Phase B as the production target-VMAF backend.
- [ADR-0287](0287-vmaf-tune-saliency-aware-encoding.md) — saliency-aware
  encoding; consumes the same predicate seam.
- [ADR-0295](0295-vmaf-tune-phase-e-bitrate-ladder.md) — Phase E ladder;
  default sampler composes Phase B with `recommend.pick_target_vmaf`.
- [ADR-0296](0296-vmaf-roi-saliency-weighted.md) — adapter
  `quality_range` is the search-space boundary.
- [ADR-0306](0306-vmaf-tune-coarse-to-fine.md) — coarse-to-fine grid
  search; complementary, not a replacement.
- [Research-0090 — Phase B bisect feasibility](../research/0090-vmaf-tune-phase-b-bisect-feasibility.md).
- Source: `req` (direct user instruction in this session: "Implement
  vmaf-tune Phase B (target-VMAF bisect) in the VMAFx/vmafx fork").
