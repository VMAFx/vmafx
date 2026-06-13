<!-- markdownlint-disable MD060 -->
# Research Digest 0613: Cross-Shot Complexity Weighting

**Scope**: Title-level quality targets (e.g. "average VMAF ≥ 94, no shot below
91") instead of per-shot independent targets; design of the constraint solver
and integration into `per_shot.py`'s predicate API.
**Retrieved**: 2026-05-19
**Status**: Planning-only; no implementation.

---

## Problem Statement

The current Phase D implementation (`per_shot.py`) treats each shot
independently: every shot is bisected to the same per-shot VMAF target
(e.g. 93.0). This is suboptimal in two ways:

1. **Bit waste on simple shots**: a simple fade-to-black or talking-head shot
   can achieve VMAF 96 at CRF 28 when the target is 93. The extra quality
   costs bits without perceptual value.
2. **Hard shots may undershoot**: a complex action sequence may barely reach
   93 at CRF 22, consuming many bits. Relaxing the target for this shot to
   91 (still above a minimum floor) while tightening other shots recovers bits
   to spend on the hard shot's detail.

The desired model: assign per-shot CRFs such that the title-level VMAF
distribution satisfies:

- `mean(vmaf) ≥ target_mean` (e.g. 94)
- `min(vmaf) ≥ floor` (e.g. 91)
- `total_bits ≤ budget` (optional hard constraint)

---

## Literature

There is no standard public paper on per-shot cross-shot weighting specifically.
The problem is related to:

- **Rate-distortion theory**: optimal bit allocation across frames (Lagrangian
  rate control). Classic result: allocate bits in proportion to complexity.
- **Netflix per-title encoding** (Tech Blog 2015; SSL failed 2026-05-19):
  implies per-title global quality targets; the per-shot extension is
  fork-local.
- **Lagrangian quality optimisation**: well-studied in H.264/HEVC rate
  control literature (e.g. HRD-compliant video coding). The Lagrange
  multiplier λ connects bitrate and distortion; adjusting λ per shot
  achieves optimal bit allocation for a fixed total budget.

---

## Current Fork State

| Component | Status |
|-----------|--------|
| `per_shot.py` `PredicateFn` | Per-shot target VMAF, no global constraint |
| `bisect.py` | Target VMAF as scalar per call |
| `conformal.py` / `uncertainty.py` | Interval-aware quality |
| Cross-shot quality optimiser | **Not implemented** |

The `PredicateFn` signature is `(shot, target_vmaf, encoder) → (crf, vmaf)`.
To support cross-shot weighting, the predicate must either:

- Accept a per-shot target derived from a global constraint solver, or
- Be replaced by a two-pass architecture: first pass estimates per-shot
  complexity; second pass solves the allocation problem; third pass runs
  bisect with per-shot targets.

---

## Design Options

### Option A: Proportional complexity-based target relaxation

Measure per-shot complexity (spatial entropy + motion energy). Assign per-shot
VMAF targets such that `target_i = target_mean - k × (complexity_i - mean_complexity)`.
Simple linear relaxation: complex shots get a lower target, simple shots
get a higher target. The k parameter controls trade-off aggressiveness.

**Pros**: Zero solver overhead; very fast; simple to implement.
**Cons**: Not guaranteed to satisfy the mean/floor constraints exactly; k must
be tuned per codec/content type.

### Option B: Iterative redistribution (greedy two-pass)

First pass: bisect all shots to a uniform target T. Measure actual VMAF scores.
Second pass: identify shots where VMAF >> T (simple shots); reduce their targets;
add the recovered bits to a "budget" that is redistributed to hard shots
(VMAF < T). Repeat until convergence or max iterations.

**Pros**: No solver; reuses existing `bisect.py`; guaranteed floor unless
content is uniformly hard.
**Cons**: Requires 2× encode overhead; convergence not guaranteed; order-
dependent.

### Option C: Lagrangian optimisation (recommended for V1)

Model the problem as: minimise `Σ bits(shot_i, crf_i)` subject to
`mean(vmaf(shot_i, crf_i)) ≥ target_mean` and `vmaf(shot_i, crf_i) ≥ floor ∀ i`.
Use a bisect on the Lagrange multiplier λ: at each λ, each shot independently
minimises `bits_i + λ × (target_vmaf - vmaf_i)`. The λ bisect converges in
~10 iterations; the inner per-shot bisect is already Phase B.

**Pros**: Principled; provably optimal under the model; reuses Phase B.
**Cons**: Requires CRF-to-bitrate and CRF-to-VMAF to be available without
full encode (use NR proxy for the inner loop if Item 3 is available); O(shots
× λ iterations) probes.

### Option D: LP/QP solver (exact)

Enumerate a discrete CRF grid for each shot; formulate as an integer program
where each shot selects one CRF; solve with scipy.optimize.linprog or
PuLP. Exact solution; tractable for ≤100 shots and ≤10 CRF levels per shot.

**Pros**: Exact; off-the-shelf solver.
**Cons**: Requires pre-computing all (shot, CRF) → (bits, VMAF) pairs —
expensive without NR proxy; scipy dependency.

---

## Recommended Decision Matrix

| Option | Optimality | Runtime cost | Dependencies |
|--------|-----------|--------------|--------------|
| A — linear relaxation | Low | Negligible | None |
| B — iterative redistribution | Medium | 2× encode | None |
| C — Lagrangian (recommended) | High | ~1.5× encode | Item 3 (NR) |
| D — LP/QP solver | Exact | 5–10× encode | Item 3 (NR) |

---

## API Design Sketch

```python
@dataclasses.dataclass
class TitleQualityConstraints:
    target_mean_vmaf: float = 94.0
    floor_vmaf: float = 91.0
    max_iterations: int = 3      # redistribution passes
    lambda_bisect_iterations: int = 10

def tune_per_shot_with_constraints(
    shots: list[Shot],
    predicate: PredicateFn,
    constraints: TitleQualityConstraints,
    encoder: str,
) -> list[ShotRecommendation]: ...
```

The `constraints` object is optional: if `None`, the existing per-shot
independent behaviour is preserved (backward compatible).

---

## Open Questions

1. Should the floor constraint be a hard lower bound (abort encode if
   violated) or a soft penalty (accept with warning)?
2. How should the constraint solver handle shots where even CRF=0 (lossless)
   cannot reach `floor_vmaf`? (Content that is too degraded to reconstruct.)
3. Is the `target_mean` a simple arithmetic mean, or a duration-weighted mean
   (longer shots contribute more to the title quality)?

---

## References

- `tools/vmaf-tune/src/vmaftune/per_shot.py` — Phase D predicate API.
- `tools/vmaf-tune/src/vmaftune/bisect.py` — Phase B CRF bisect.
- `tools/vmaf-tune/src/vmaftune/uncertainty.py` — confidence intervals.
- Lagrangian rate control: ITU-T SG16 standard rate-control literature
  (H.264 / H.265 HRD compliance); no public arXiv paper on per-shot VMAF
  Lagrangian found in search 2026-05-19.
- ADR-0617 — Decision record for cross-shot complexity weighting.
