<!-- markdownlint-disable MD060 -->
# ADR-0614: Per-Shot ABR Rendition Selection

- **Status**: Proposed
- **Date**: 2026-05-19
- **Deciders**: lusoris
- **Tags**: `ai`, `planning`, `vmaf-tune`

## Context

The Phase E ladder produces a per-title convex-hull set of (resolution, bitrate)
knees. The Phase D per-shot tuner picks a CRF per shot but always encodes at
source resolution. The cross-product — assigning each shot a (resolution, CRF)
pair drawn from the per-title ladder — is absent. A title with 1000 shots has
shots whose optimal encoding resolution may differ by 2–3 ladder rungs;
forcing all shots to source resolution wastes bits on simple shots and may
under-serve complex ones at lower rungs. Per-shot ABR rendition enables DASH
packaging where each segment can be at a different resolution.

## Decision

We will implement per-shot VMAF-targeted rung selection (Research 0610 Option B):
a new `rendition_picker.py` evaluates candidate ladder rungs for each shot using
the Phase B bisect predicate and selects the lowest-bitrate rung meeting the
shot's VMAF target. The output is an extended `ShotRecommendation` that includes
the selected ladder rung. DASH resolution clustering is scoped as a follow-on
phase (P3) and deferred.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| A — complexity-threshold fixed rung | Zero encode overhead | Hand-tuned thresholds; coarse | Accuracy insufficient for production |
| B — VMAF-targeted rung selection (chosen) | Content-adaptive; no hand-tuning | O(rungs × shots) probes | Mitigated by ADR-0615 (NR) |
| C — learned per-shot predictor | Zero runtime overhead | Training data + ADR-0618 dependency | Deferred post classifier |
| D — joint DO-extended optimisation | Global optimum | Highest complexity; requires ADR-0613 | Deferred |

## Consequences

- **Positive**: 5–15% estimated BD-rate improvement on heterogeneous titles;
  each shot encodes at its natural resolution sweet spot.
- **Negative**: DASH packaging complexity increases; HLS backwards compatibility
  requires explicit handling (constant resolution per HLS variant stream).
- **Neutral / follow-ups**: Packaging support (`packaging.py` or Shaka Packager
  integration) must follow in P3.

## Dependencies

- **ADR-0295** (`ladder.py` Phase E) — must be stable; rung set is the input.
- **ADR-0615** (Fast NR pre-scoring) — optional; reduces O(rungs × shots) to
  O(rungs + 1 FR call) per shot.
- **ADR-0613** (Dynamic Optimizer) — optional; can be extended to joint
  shot-rung-CRF optimisation, not required for initial rung picker.

## Implementation phases

| Phase | Description | Effort |
|-------|-------------|--------|
| P1 | `rendition_picker.py`; per-shot rung evaluation; unit tests | 2 days |
| P2 | Integration with `tune_per_shot` and `merge_shots`; extended `EncodingPlan` | 1 day |
| P3 | DASH resolution clustering; multi-Representation output | 2 days |
| P4 | CLI flag `--per-shot-resolution`; docs | 0.5 day |
| P5 (post ADR-0615) | Switch probes to NR oracle; validate speedup | 1 day |

Total estimate: 5.5–6.5 days (excluding P5).

## References

- Research digest: [docs/research/0610-per-shot-abr-rendition-research.md](../research/0610-per-shot-abr-rendition-research.md).
- arXiv:2408.01932 — Durbha & Bovik, 2024 (retrieved 2026-05-19).
- arXiv:2512.12952 — Durbha et al., 2025 (retrieved 2026-05-19).
- `tools/vmaf-tune/src/vmaftune/ladder.py`, `per_shot.py`.
- Source: per user direction (roadmap planning session 2026-05-19).
