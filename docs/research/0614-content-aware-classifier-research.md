<!-- markdownlint-disable MD060 -->
# Research Digest 0614: Content-Aware Classifier

**Scope**: Pre-encoding classifier that tags source video with genre, motion
intensity, scene complexity, dynamic range, colour characteristics, source
quality, and subjective tags; feeds per-content-type encoder routing.
**Retrieved**: 2026-05-19
**Status**: Planning-only; no implementation.

---

## Problem Statement

Different content types require different encoding strategies:

- **Animation 2D**: compresses well at low resolution; encoder `tune=animation`
  is beneficial; NEG VMAF inappropriate (no sharpening).
- **Sports/live-action**: high motion, fine detail; needs higher CRF headroom;
  per-shot boundaries are frequent.
- **Talking-head/dialogue**: low spatial complexity; very compressible; a flat
  CRF 28 may suffice for 99% of the title.
- **HDR / Dolby Vision**: requires HDR-aware VMAF models; different bit-depth
  handling.

A content classifier that runs once per clip (10 seconds → tag dict) enables:

- Routing to per-content-type VMAF priors and ladder defaults.
- Selecting encoder `tune` params automatically.
- Adjusting per-shot bisect thresholds (e.g. tighter NR uncertainty zone for
  sports vs animation).

---

## Required Tag Set

| Tag category | Values |
|-------------|--------|
| `genre` | `live-action`, `animation-2d`, `animation-3d`, `sports`, `talking-head`, `mixed`, `unknown` |
| `motion_intensity` | `low`, `medium`, `high` |
| `scene_complexity` | `simple`, `medium`, `complex` |
| `dynamic_range` | `sdr`, `hdr-10`, `hdr-hlg`, `dolby-vision` |
| `color_profile` | `standard`, `neon`, `muted`, `high-contrast` |
| `source_quality` | `studio-master`, `streaming-re-encode`, `amateur`, `archival` |
| `subjective` | list: `dialogue-heavy`, `action-heavy`, `atmospheric`, etc. |

`dynamic_range` can be inferred deterministically from container metadata
(no ML needed). Other tags require ML or signal analysis.

---

## Existing Primitives Survey

### CAMBI (in-tree)

CAMBI (Contrast Aware Multiscale Banding Index) is a no-reference banding
detector. It can detect quantisation-induced contouring artifacts, which is
a proxy for `source_quality: streaming-re-encode`. However, CAMBI was
designed for banding detection, not complexity classification.

### FFmpeg `siti` filter

FFmpeg's `siti` filter computes the ITU-T P.910 Spatial Information (SI) and
Temporal Information (TI) metrics. SI ∝ spatial complexity; TI ∝ motion
intensity. Both can be extracted with a single `ffprobe`/`ffmpeg` pass at
low cost. This covers the `scene_complexity` and `motion_intensity` tags
without any ML.

### OpenCV / MediaPipe

MediaPipe's MediaPipe Video Classification solution (MediaPipe Tasks,
released 2023) runs EfficientNet-based multi-label video classification on
device. Pre-trained on Kinetics-700; covers genre-level tags (sports,
dancing, cooking) but not `animation-2d` vs `animation-3d`.

OpenCV does not include a built-in content classifier, but provides
feature extraction primitives (optical flow, histogram, DCT).

### Anthropic Claude Vision API

The Claude Vision API (Anthropic SDK; `claude-sonnet-4-5` or later) can
accept video frame grabs and return structured JSON tag dicts via tool use.
Example prompt: "Given these 3 frames sampled at 25%, 50%, 75% of this
video clip, output a JSON dict with keys: genre, motion_intensity,
scene_complexity, dynamic_range, color_profile, source_quality."

**Pros**: Zero training required; natural language + zero-shot; handles
edge cases gracefully; the fork already integrates the Anthropic SDK.
**Cons**: Network latency (~1–2 s per call); API cost ($); privacy concern
for unreleased content; non-deterministic (LLM-sampled).

### VLM-based (Ollama — Gemma Vision / Llama Vision)

Local VLM inference via Ollama. Gemma 3B Vision or Llama 3.2 11B Vision can
run on the dev machine's RTX 4090 (the fork already uses Ollama for dev-llm
skills). Zero API cost; on-premises; comparable accuracy to cloud VLM on
structured tagging tasks.

**Pros**: On-premises; no API cost; GPU-accelerated; deterministic at fixed
temperature.
**Cons**: Requires Ollama running with appropriate model pulled; adds GPU
dependency to a formerly CPU-only pre-processing step; 2–5 s per call on
RTX 4090 for 11B model.

---

## Design Options (A1–A4)

### A1: VLM-based (Gemma / Llama Vision via Ollama)

3 frames sampled at 25%/50%/75% → Ollama VLM → JSON tag dict. One call per clip.

**Pros**: Zero training; handles all tag categories; handles edge cases.
**Cons**: Ollama dependency; GPU required; 2–5 s latency; non-deterministic.

**Pros/Cons table**:

| Criterion | Score |
|-----------|-------|
| Training needed | None |
| Runtime cost | 2–5 s / clip |
| Coverage | All tags |
| Determinism | No (sample-based) |
| Privacy | On-premises with Ollama |

### A1b: Claude Vision API (Anthropic cloud)

Same as A1 but uses the Claude API instead of local Ollama.

| Criterion | Score |
|-----------|-------|
| Training needed | None |
| Runtime cost | 1–3 s / clip + network |
| Coverage | All tags |
| Determinism | No |
| Privacy | Content leaves premises |

Not recommended for unreleased content.

### A2: Train small CNN classifier

Train a MobileNetV3 or EfficientNet-B0 on labeled video clips. Labels can
come from Activity-Net, Moments-in-Time, or the fork's Netflix corpus
(manually labeled). Output: `genre`, `motion_intensity`, `scene_complexity`.

| Criterion | Score |
|-----------|-------|
| Training needed | 1–2 weeks |
| Runtime cost | <100 ms / clip (CPU) |
| Coverage | genre + motion + complexity only |
| Determinism | Yes |
| Privacy | Fully on-premises |

### A3: CAMBI + FFmpeg SI/TI as proxy (no ML)

Use `ffmpeg -vf siti` for SI (spatial complexity) and TI (temporal / motion).
Use CAMBI score as `source_quality` proxy.
`dynamic_range` from container metadata.

| Criterion | Score |
|-----------|-------|
| Training needed | None |
| Runtime cost | 5–10 s / clip (single ffmpeg pass) |
| Coverage | scene_complexity + motion_intensity + dynamic_range + source_quality |
| Determinism | Yes |
| Privacy | Fully on-premises |
| Genre coverage | None (can't distinguish animation from live-action) |

### A4: Hybrid (A3 for cheap proxies + A1 for genre/semantic only) — Recommended

Run A3 first (deterministic, cheap). If `genre` or `source_quality` tags are
needed for routing (e.g. to switch to `tune=animation`), run one VLM call
(A1) only for those missing tags. Cache results per clip fingerprint.

| Criterion | Score |
|-----------|-------|
| Training needed | None |
| Runtime cost | 5–10 s (A3) + optional 2–5 s (A1) |
| Coverage | All tags |
| Determinism | Partially |
| Privacy | On-premises (with Ollama) |

---

## Per-Content Routing Table (design sketch)

```python
ENCODER_ROUTING = {
    "animation-2d":   {"tune": "animation", "vmaf_model": "vmaf_v0.6.1", "neg": False},
    "animation-3d":   {"tune": "animation", "vmaf_model": "vmaf_v0.6.1", "neg": True},
    "sports":         {"tune": None,         "vmaf_model": "vmaf_v0.6.1", "neg": False},
    "talking-head":   {"tune": None,         "vmaf_model": "vmaf_v0.6.1", "neg": False},
    "live-action":    {"tune": None,         "vmaf_model": "vmaf_v0.6.1", "neg": False},
}

MOTION_LADDER_PRIORS = {
    "high":   {"target_vmaf": 93, "min_floor": 90},
    "medium": {"target_vmaf": 94, "min_floor": 91},
    "low":    {"target_vmaf": 95, "min_floor": 92},
}
```

The routing table feeds `ladder.py`'s `SamplerFn` seam and `per_shot.py`'s
`PredicateFn` default parameters.

---

## Dataset Options for A2 Training

| Dataset | Size | Genre labels | Motion | Available |
|---------|------|-------------|--------|-----------|
| Activity-Net v1.3 | 20,000 clips | Yes (200 classes) | Implicit | Public |
| Moments-in-Time | 3M clips | Yes (339 classes) | Implicit | Registration |
| Kinetics-700 | 700,000 clips | Yes (700 classes) | Implicit | Public |
| Fork's Netflix corpus | 79 clips | None (manual label needed) | Implicit | Local |

For fork training, mapping Activity-Net / Kinetics classes to our coarser
tag set (7 genres) requires a taxonomy crosswalk table.

---

## Open Questions

1. Is on-premises VLM (Ollama) acceptable as a runtime dependency for
   `vmaf-tune`, or must the classifier be usable without a GPU?
2. Should `dynamic_range` always be inferred from container metadata, or
   can VLM help for JPEG/PNG stills where no container metadata exists?
3. What is the minimum clip length for reliable classification? (10 s is
   the standard, but clips from the `per_shot.py` pipeline may be < 1 s.)
4. How should the tag dict be cached and invalidated? (Content fingerprint
   via xxHash128 of the first 1 MB of the file?)

---

## References

- MediaPipe Video Classification documentation (mediapipe.dev; 2023).
- FFmpeg `siti` filter: ITU-T P.910 SI/TI computation.
- CAMBI documentation: `resource/doc/cambi.md` (Netflix/vmaf upstream,
  via GitHub API 2026-05-19).
- Anthropic Claude Vision API (claude.ai/docs; 2025 — in-tree SDK dependency).
- Activity-Net v1.3: crcv.ucf.edu/research/activity-net (2016).
- arXiv:2408.01932 — confirms per-shot complexity variation as motivation.
- ADR-0618 — Decision record for content-aware classifier.
