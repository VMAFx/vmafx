<!-- markdownlint-disable MD060 -->
# Research Digest 0609: Dynamic Optimizer (DO)

**Scope**: Joint shot-boundary + CRF co-optimization for per-shot encoding.
**Retrieved**: 2026-05-19
**Status**: Planning-only; no implementation.

---

## Background and Literature

### Netflix Dynamic Optimizer (primary reference)

Netflix published the Dynamic Optimizer (DO) framework via their Tech Blog.
The blog post "Dynamic Optimizer: A Perceptual Video Encoding Optimization
Framework" (netflixtechblog.com, circa 2018, SSL error prevented direct
retrieval 2026-05-19) describes a system that:

- Analyses the source video's complexity and temporal variation frame-by-frame
  before encoding.
- Identifies shot boundaries to isolate perceptually homogeneous segments.
- For each shot, selects the optimal (resolution, CRF) pair by walking the
  rate-quality convex hull rather than scanning a fixed grid.
- Uses VMAF as the perceptual quality oracle throughout.

The key insight is that shot boundaries and CRF choices are *coupled*: a
slightly different cut point can move frames with very different complexity
into the same shot, requiring a more conservative (lower) CRF to protect
the hard frames — thus wasting bits. DO co-optimises the two decisions.

### Per-Shot Bitrate Ladder Research

Durbha & Bovik (2024), "Constructing Per-Shot Bitrate Ladders using Visual
Information Fidelity", arXiv:2408.01932, IEEE Trans. Image Processing
doi:10.1109/TIP.2025.3625750. Independently confirms the DO approach:
per-shot convex-hull construction with VIF features avoids exhaustive
encoding while achieving competitive quality. The paper validates that
VMAF + per-shot segmentation is the industry baseline.

### Related: Live-Streaming Dynamic Ladders

Xiong et al. (2026), "Dynamic resolution switching for live streaming",
arXiv:2605.15490, accepted ICIP 2026. Extends the DO concept to live
streaming using a lightweight bitstream-based VQM; achieves ~9% BD-rate
reduction. Notes that offline per-title convex-hull construction is
impractical for live — relevant for our roadmap because it bounds the
use case.

---

## Current Fork State

| Component | Status |
|-----------|--------|
| `per_shot.py` | TransNet V2 shot detection, pluggable `PredicateFn` per shot |
| `ladder.py` | Convex-hull ladder with uncertainty-aware rung selection |
| `bisect.py` | Phase B binary CRF search using full FR VMAF |
| Dynamic Optimizer joint co-optimisation | **Not implemented** |

The current pipeline is: detect shots → bisect each shot independently.
The shot boundaries are treated as ground truth from TransNet V2; the
per-shot CRF is picked greedily without regard to neighbouring shots.

---

## Design Options

### Option A: Post-TransNet boundary refinement (boundary tweak only)

After TransNet outputs candidate boundaries, slide each boundary ±N frames
and re-evaluate whether the adjacent shot pair's combined CRF cost improves.
Complexity: O(N × shots). No changes to `ladder.py`.

**Pros**: Minimal invasiveness; fits into existing `per_shot.py` API.
**Cons**: Only local refinement; does not jointly optimise boundary + CRF globally.

### Option B: Complexity-map-guided segmentation (replace TransNet cuts)

Pre-compute a per-frame complexity signal (motion entropy + DCT variance).
Use a 1D dynamic programming segmentation to find the partition into k shots
that minimises within-shot CRF variance. Boundaries no longer come from
TransNet's learned temporal model.

**Pros**: Explicitly targets encoding cost.
**Cons**: Discards TransNet's semantically meaningful cuts; may produce odd
segment boundaries from a playback perspective; loses the scene-cut semantic.

### Option C: TransNet cuts + DO post-pass with fixed budget (recommended)

Keep TransNet cuts as the primary segmentation. After the initial per-shot
CRF selection, run a second pass: for each adjacent shot pair, evaluate
whether merging them (or splitting at a different frame) reduces total bit
cost for the same average VMAF target. Terminate after one round-trip.
This is the closest approximation to Netflix's published DO without full
joint optimisation.

**Pros**: Incremental over Phase D; reuses existing `bisect.py`; semantically
meaningful boundaries remain the default.
**Cons**: Two-pass complexity; does not globally optimise; single-round heuristic
may miss multi-shot interactions.

### Option D: Full joint optimisation (DP over shot graph)

Model the problem as a 1D DP where each state is a (frame, crf) pair and the
transition cost is the bitrate delta for that segment at that CRF. Solve for
the partition + CRF assignment that satisfies a global VMAF floor at minimum
bitrate. Exact; potentially exponential but tractable with a bounded shot
count (≤200 cuts/title typical).

**Pros**: Mathematically optimal within the model.
**Cons**: Requires a fast CRF-to-VMAF predictor (NR or FR) to make the DP
tractable; couples to Item 3 (Fast NR pre-scoring); significant implementation
complexity; overkill for most titles.

---

## Recommended Decision Matrix

| Option | Quality gain | Implementation cost | Dependency risk |
|--------|-------------|---------------------|-----------------|
| A — boundary tweak | Low | 1 day | None |
| B — complexity segmentation | Medium | 3 days | Replaces TransNet |
| C — DO post-pass (recommended) | Medium-high | 3–5 days | Item 3 (NR) optional |
| D — full DP | High | 2–3 weeks | Items 3 + 5 |

---

## Key Open Questions

1. Should DO use NR pre-scoring (Item 3) to evaluate candidate boundaries
   cheaply, or pay the full FR VMAF cost for each candidate merge?
2. What is the maximum allowed shot-boundary drift from TransNet's output?
   (Semantic scene cuts matter for downstream chapter markers / ad break logic.)
3. Is a global VMAF floor (Item 5) a prerequisite for DO, or can DO run
   with per-shot targets only?

---

## References

- Netflix Tech Blog "Dynamic Optimizer" post (URL: netflixtechblog.com/dynamic-optimizer-…;
  direct retrieval failed 2026-05-19 — SSL error from this host).
- arXiv:2408.01932 — Durbha & Bovik, "Constructing Per-Shot Bitrate Ladders
  using Visual Information Fidelity", IEEE TIP 2025. Retrieved 2026-05-19.
- arXiv:2605.15490 — Xiong et al., "Dynamic resolution switching for live
  streaming", ICIP 2026. Retrieved 2026-05-19.
- `tools/vmaf-tune/src/vmaftune/per_shot.py` — Phase D implementation.
- `tools/vmaf-tune/src/vmaftune/bisect.py` — Phase B CRF bisect.
- ADR-0613 — Decision record for DO integration.
