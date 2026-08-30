<!-- markdownlint-disable MD060 -->
# Research Digest 0610: Per-Shot ABR Rendition Selection

**Scope**: Assign each shot a (resolution, bitrate) rendition drawn from the
per-title convex-hull ladder, rather than encoding all shots at source
resolution with only a varying CRF.
**Retrieved**: 2026-05-19
**Status**: Planning-only; no implementation.

---

## Background and Literature

### Netflix Per-Title Encoding

Netflix's seminal work "Per-Title Encode Optimization" (Tech Blog, 2015;
direct SSL retrieval failed 2026-05-19) introduced the idea that the
optimal ABR ladder differs per title. The convex hull of (resolution ×
bitrate → VMAF) points varies dramatically between an animated feature
(compresses well at low resolution) and a live-action action film (needs
higher resolution to preserve detail).

### Per-Shot Extension

The natural extension is to apply per-title convex-hull logic at shot
granularity. A single 2-hour film may contain 500–2000 shots that differ
in motion intensity, texture complexity, and spatiotemporal entropy. A
uniform ladder wastes bits on simple shots and under-serves complex ones.

Durbha & Bovik (2024), arXiv:2408.01932, validates the approach: per-shot
bitrate ladder construction with VIF features achieves competitive
quality vs exhaustive convex-hull construction, confirming that per-shot
ladder differentiation is viable without full re-encoding of every candidate.

Durbha et al. (2025), arXiv:2512.12952, "Leveraging Compression to
Construct Transferable Bitrate Ladders", further demonstrates ML-based
prediction of convex hulls from source features, avoiding exhaustive
per-shot encode sweeps entirely.

### Relationship to HLS/DASH Packaging

Per-shot renditions break the assumption that all segments of one
"representation" have the same resolution. DASH allows different
resolutions per Representation across AdaptationSets, but HLS `#EXT-X-STREAM-INF`
requires consistent resolution per variant stream. Packaging constraints
must be respected; the encoding plan may need to up-scale some shots to
maintain a consistent ladder presentation at the player level.

---

## Current Fork State

| Component | Status |
|-----------|--------|
| `ladder.py` | Per-title convex hull, uncertainty-aware rung selection |
| `per_shot.py` | Per-shot CRF selection at source resolution only |
| Per-shot resolution switching | **Not implemented** |
| DASH/HLS multi-resolution packaging | **Not implemented** |

The Phase E ladder produces a per-title set of (resolution, bitrate) knees.
The Phase D per-shot tuner picks a CRF for each shot at source resolution.
The cross-product — selecting a per-shot (resolution, CRF) pair from the
per-title ladder — is the gap.

---

## Data Flow Design

```text
Source video
    │
    ▼
[per-title ladder.py]
    │   → ladder: [(res1, crf1), (res2, crf2), ...]  (convex hull knees)
    ▼
[per-shot TransNet V2 boundary detection]
    │   → shot list: [Shot(0, 240), Shot(240, 600), ...]
    ▼
[per-shot complexity estimator]
    │   → shot complexity: {shot: complexity_score}
    ▼
[per-shot rendition picker]
    │   picks one ladder rung per shot based on complexity
    │   → shot encode plan: {shot: (resolution, crf)}
    ▼
[FFmpeg multi-resolution encode plan]
    │   encodes each shot segment at its assigned resolution + CRF
    ▼
[packaging: upscale-to-max-res + concat, or DASH multi-rep]
```

---

## Design Options

### Option A: Fixed-rung per-shot assignment (complexity threshold)

Each shot is assigned the ladder rung that best matches its pre-measured
complexity score. A simple lookup table: complexity ∈ [low, medium, high]
→ rung 1, 2, 3. No per-shot encode sweep.

**Pros**: Zero encode overhead beyond complexity measurement.
**Cons**: Crude; misses title-specific calibration; requires complexity
thresholds to be hand-tuned or learned.

### Option B: Per-shot VMAF-targetted rung selection (recommended)

For each shot, run a fast VMAF score (NR or subsampled FR) at each candidate
rung resolution and CRF. Pick the lowest-bitrate rung that meets the shot's
VMAF target. Reuses `bisect.py`'s predicate API.

**Pros**: Content-adaptive; no hand-tuned thresholds; compatible with
existing predicate API.
**Cons**: O(rungs × shots) encode probes; mitigated by Item 3 (NR pre-scoring)
which can short-circuit most probes.

### Option C: Learned per-shot rung predictor

Train an ML model (lightweight CNN or gradient-boosted tree) on source
features → optimal rung assignment. Offline training required.

**Pros**: Zero runtime encode overhead; very fast per-shot decision.
**Cons**: Training data requirement; generalisation risk on unseen content types;
couples tightly to Item 6 (content classifier).

### Option D: Joint rung + CRF optimisation (DO-extended)

Extend Option D from the DO roadmap item to jointly optimise (shot boundary,
resolution, CRF). The state space is larger but conceptually identical.

**Pros**: Global optimum.
**Cons**: Highest complexity; requires Item 1 (DO) to be implemented first.

---

## Recommended Decision Matrix

| Option | Quality gain | Runtime overhead | Dependency |
|--------|-------------|-----------------|------------|
| A — complexity threshold | Medium | Negligible | Item 6 (complexity proxy) |
| B — VMAF-targeted (recommended) | High | 2–5× vs current | Item 3 (NR) |
| C — learned predictor | High | ~Zero | Item 6 + training corpus |
| D — joint DO-extended | Highest | 5–10× | Items 1, 3, 5 |

---

## Packaging Constraint Analysis

HLS requires consistent resolution per variant. Options:

1. **Segment-level resolution lock**: all segments of a given representation
   encode at the same resolution; some shots are upscaled.
2. **DASH AdaptationSet per resolution**: full DASH packaging, each resolution
   is its own Representation; player switches resolution per segment.
3. **Resolution clustering**: assign each shot to the nearest ladder rung
   resolution; cluster shots sharing the same resolution into a
   DASH Representation.

Option 3 is the recommended path: it preserves DASH standards compliance
and avoids upscaling overhead.

---

## Open Questions

1. Does the player (e.g. Shaka, ExoPlayer) support per-segment resolution
   switches within a single Representation, or must we use separate
   AdaptationSets?
2. What is the minimum shot length for a resolution switch to be worth the
   packaging overhead? (Shots < 1 second: probably not worth it.)
3. Should the per-shot rendition picker share the complexity estimator with
   Item 6 (content classifier), or maintain a separate lightweight proxy?

---

## References

- Netflix Tech Blog "Per-Title Encode Optimization" (2015; SSL retrieval
  failed 2026-05-19).
- arXiv:2408.01932 — Durbha & Bovik, "Constructing Per-Shot Bitrate Ladders
  using Visual Information Fidelity", IEEE TIP 2025. Retrieved 2026-05-19.
- arXiv:2512.12952 — Durbha et al., "Leveraging Compression to Construct
  Transferable Bitrate Ladders", IEEE TIP (under review), Dec 2025.
  Retrieved 2026-05-19.
- `tools/vmaf-tune/src/vmaftune/ladder.py` — Phase E convex-hull ladder.
- `tools/vmaf-tune/src/vmaftune/per_shot.py` — Phase D per-shot CRF tuning.
- ADR-0614 — Decision record for per-shot ABR rendition.
