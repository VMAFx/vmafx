<!-- markdownlint-disable MD060 -->
# ADR-0617: Cross-Shot Complexity Weighting and Title-Level Quality Constraints

- **Status**: Proposed
- **Date**: 2026-05-19
- **Deciders**: lusoris
- **Tags**: `ai`, `planning`, `vmaf-tune`

## Context

The current Phase D bisect assigns the same VMAF target to every shot. Simple
shots (fade-to-black, talking-head) achieve well above target at the same CRF,
wasting bits. Complex shots (action sequences) may barely reach target, under-
serving detail. A title-level constraint — "average VMAF ≥ 94, no shot below 91"
— allows bit redistribution: surplus bits from easy shots are reallocated to hard
shots, improving the title's quality per byte without increasing total bitrate.

## Decision

We will implement a Lagrangian quality optimiser (Research 0613 Option C):
a bisect on the Lagrange multiplier λ where at each λ, each shot independently
minimises `bits_i + λ × max(0, target_mean - vmaf_i)`. A new
`TitleQualityConstraints` dataclass and `tune_per_shot_with_constraints`
function are added to `per_shot.py`. The API is backward-compatible: passing
`constraints=None` preserves the existing independent per-shot behaviour.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| A — linear complexity relaxation | Zero solver overhead; fast | Not guaranteed to satisfy mean/floor constraints | Insufficient for production quality targets |
| B — iterative redistribution | No solver; reuses bisect | 2× encode overhead; convergence not guaranteed | Order-dependent; less principled |
| C — Lagrangian optimisation (chosen) | Principled; reuses Phase B; provably optimal under model | O(shots × λ iters) probes; NR proxy needed for speed | — |
| D — LP/QP exact solver | Exact solution | Pre-compute all (shot, CRF) pairs; scipy dependency | Overkill; deferred if Lagrangian proves insufficient |

## Consequences

- **Positive**: Titles with heterogeneous shot complexity achieve better quality
  per byte without changing total bitrate; the per-shot floor constraint
  eliminates quality cliff artifacts.
- **Negative**: O(shots × λ-bisect-iterations) encode overhead; without NR proxy
  (ADR-0615) each λ evaluation requires full FR scoring.
- **Neutral / follow-ups**: The `target_mean` must be defined as duration-weighted
  (longer shots carry more weight); `floor` violation handling (content that cannot
  reach floor even at CRF=0) needs a documented soft-fail path.

## Dependencies

- **ADR-0615** (Fast NR pre-scoring) — strongly recommended; without it each
  λ-bisect inner iteration pays full FR cost.
- **Phase D** (`per_shot.py`) must be stable.
- **ADR-0613** (Dynamic Optimizer) — complementary; DO optimises boundaries,
  cross-shot weighting optimises targets. The two can be pipelined.

## Implementation phases

| Phase | Description | Effort |
|-------|-------------|--------|
| P1 | `TitleQualityConstraints` dataclass; `tune_per_shot_with_constraints` skeleton | 1 day |
| P2 | Lagrangian λ bisect over per-shot targets; unit tests with mock bisect | 2 days |
| P3 | Duration-weighted mean; floor soft-fail path; integration tests | 1 day |
| P4 | CLI flags `--target-mean-vmaf`, `--floor-vmaf`; docs | 0.5 day |

Total estimate: 4.5 days.

## References

- Research digest: [docs/research/0613-cross-shot-complexity-weighting-research.md](../research/0613-cross-shot-complexity-weighting-research.md).
- `tools/vmaf-tune/src/vmaftune/per_shot.py`, `bisect.py`, `uncertainty.py`.
- Lagrangian rate control: H.264 / H.265 HRD standard literature.
- Source: per user direction (roadmap planning session 2026-05-19).
