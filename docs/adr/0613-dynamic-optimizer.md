<!-- markdownlint-disable MD060 -->
# ADR-0613: Dynamic Optimizer — Joint Shot-Boundary + CRF Co-Optimisation

- **Status**: Proposed
- **Date**: 2026-05-19
- **Deciders**: lusoris
- **Tags**: `ai`, `planning`, `vmaf-tune`

## Context

The current per-shot tuning pipeline (Phase D, `per_shot.py`) treats shot
boundaries as fixed outputs of TransNet V2 and bisects each shot's CRF
independently. Netflix's Dynamic Optimizer framework co-optimises both
decisions: the cut point and the CRF are jointly selected to minimise
total bitrate while satisfying a VMAF target. This yields better bit
allocation because a slightly different boundary placement can prevent
a single complex frame from forcing a very low CRF across a long, otherwise
simple shot. The gap is that the fork's pipeline is greedy-sequential:
detect shots, then bisect each shot in isolation.

## Decision

We will implement a post-TransNet Dynamic Optimizer pass (Research 0609
Option C) that evaluates boundary-merge and boundary-shift candidates for
each adjacent shot pair, using the existing Phase B bisect as the inner
quality oracle. The implementation lives in a new `dynamic_optimizer.py`
module under `tools/vmaf-tune/src/vmaftune/`. The full-DP joint optimisation
is deferred until ADR-0615 (Fast NR pre-scoring) provides a cheap inner
oracle.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| A — post-TransNet boundary tweak only | Minimal; no new module | Local-only; misses multi-shot interactions | Too limited to recover meaningful bits |
| B — complexity-map segmentation (replace TransNet) | Encoding-cost objective | Discards semantic cuts; loses chapter/ad-break alignment | Breaks downstream metadata consumers |
| C — DO post-pass (chosen) | Incremental; reuses Phase B; semantics preserved | Single-round heuristic; O(2×shots) bisect calls | — |
| D — full DP joint optimisation | Globally optimal | Requires fast oracle; high complexity | Deferred post ADR-0615 |

## Consequences

- **Positive**: 3–8% estimated BD-rate improvement on titles with
  mixed-complexity shots; no change to TransNet boundaries visible to
  downstream chapter-marker consumers.
- **Negative**: 2× encode overhead for the boundary-evaluation pass; new
  module increases maintenance surface.
- **Neutral / follow-ups**: ADR-0615 (Fast NR pre-scoring) should land first
  to make candidate evaluation cheap. ADR-0617 (cross-shot weighting) shares
  the shot complexity signal and may ship in the same feature branch.

## Dependencies

- **ADR-0615** (Fast NR pre-scoring) — optional; makes candidate evaluation
  cheap. DO can ship without it at higher runtime cost.
- **ADR-0617** (Cross-shot complexity weighting) — shares complexity signal;
  recommended to implement together.
- Phase D and Phase B must have stable APIs before DO is layered on top.

## Implementation phases

| Phase | Description | Effort |
|-------|-------------|--------|
| P1 | `dynamic_optimizer.py` skeleton; adjacent-pair merge evaluation; unit tests | 2 days |
| P2 | Boundary-shift scan (±N frames); integration with `tune_per_shot` | 1 day |
| P3 | CLI flag `--dynamic-optimizer`; docs `docs/usage/vmaf-tune-do.md` | 1 day |
| P4 (post ADR-0615) | Switch candidate evaluation to NR oracle; validate speedup | 1 day |

Total estimate: 4–5 days (excluding P4).

## References

- Research digest: [docs/research/0609-dynamic-optimizer-research.md](../research/0609-dynamic-optimizer-research.md).
- Netflix Tech Blog "Dynamic Optimizer" (netflixtechblog.com; SSL failed 2026-05-19).
- arXiv:2408.01932 — Durbha & Bovik, 2024 (retrieved 2026-05-19).
- `tools/vmaf-tune/src/vmaftune/per_shot.py`, `bisect.py`.
- Source: per user direction (roadmap planning session 2026-05-19).
