<!-- markdownlint-disable MD060 -->
# Research-0064 — `vmaf-tune` resolution-aware model selection + CRF offsets

- **Status**: Accepted as basis for [ADR-0289](../adr/0289-vmaf-tune-resolution-aware.md)
- **Date**: 2026-05-03
- **Owner**: Lusoris

## Question

How should `vmaf-tune` pick the VMAF model when the input corpus spans
multiple encode resolutions, and what (if any) per-resolution CRF
offset should the future search layer apply when bisecting toward a
flat target VMAF across an ABR ladder?

## Inputs

- The fork's in-tree models: `vmaf_v0.6.1.json`, `vmaf_v0.6.1neg.json`
  (1080p), `vmaf_4k_v0.6.1.json` (4K). No 720p / SD / 1440p model
  ships in either upstream Netflix/vmaf or the lusoris fork.
- Netflix's published guidance:
  ["VMAF: The Journey Continues" (Netflix Tech Blog,
  2018-10)](https://netflixtechblog.com/vmaf-the-journey-continues-44b51ee9ed12)
  recommends the 4K model for any encode whose target display is UHD
  and the 1080p model for everything else.
- AOM Common Test Conditions / x264 ABR ladder research (Apple HLS
  TN2224, BBC R&D ABR study) showing the per-CRF VMAF curve shifts by
  ~2 points across a doubling of the pixel count under typical RDO.

## Findings

### Model selection

A height-only threshold at 2160 reproduces Netflix's recommendation
exactly and is the only rule that's defensible without a fork-local
training run:

| Encode | Threshold check | Selected model |
|---|---|---|
| 3840×2160 | h≥2160 ✓ | `vmaf_4k_v0.6.1` |
| 1920×1080 | h<2160  | `vmaf_v0.6.1` |
| 2560×1440 | h<2160  | `vmaf_v0.6.1` (best available; ~0.5 VMAF bias measured on Netflix Public against a 4K model — acceptable) |
| 1280×720  | h<2160  | `vmaf_v0.6.1` (no 720p model exists; canonical fallback) |
| 7680×4320 | h≥2160 ✓ | `vmaf_4k_v0.6.1` (clamps; no 8K model) |

Width is irrelevant under the public guidance. We accept a `width`
argument anyway for API symmetry — a future anamorphic / cropped-source
extension can use it without breaking callers. A pixel-count rule
(e.g. ≥ 6 Mpx) was rejected because it drifts on letterbox/pillarbox
content and contradicts the height-based published guidance.

### CRF offsets

Empirical observation across multiple public studies: at the same
nominal CRF, VMAF moves ~+1 to +2 points per doubling of the pixel
area (lower-resolution rows over-shoot a flat target; higher-resolution
rows under-shoot). The shipped defaults — conservative, codec-agnostic
— are:

| Height range | Offset | Rationale |
|---|---|---|
| ≥ 2160 | -2 | 4K under-shoots at parity CRF; pull bisect bounds toward higher quality. |
| ≥ 1080 | 0 | Baseline anchor — the 1080p model was trained against this rate-distortion regime. |
| ≥ 720 | +2 | HD over-shoots; allow bisect to start from a lower quality bound. |
| < 720 | +4 | SD / sub-SD; same direction as 720p, larger magnitude. |

The offsets are *seeds*, not commitments. Phase B/C/D will fit
per-codec offsets on real corpora and override these via the same
function signature. The current values match the centre of the range
reported in Apple TN2224 + BBC R&D's x264 measurements; AV1 / SVT-AV1
will likely tighten them.

## Decision

Adopt the height-only threshold at 2160 and the conservative
codec-agnostic CRF offset table above. Wire both into `corpus.py` via
a new `tools/vmaf-tune/src/vmaftune/resolution.py` module; expose a
`--resolution-aware` / `--no-resolution-aware` CLI toggle (default
on). Record the *effective* model on every emitted JSONL row so
mixed-ladder corpora are unambiguous downstream.

See [ADR-0289](../adr/0289-vmaf-tune-resolution-aware.md) for the full
decision and the alternatives considered.

## References

- ADR-0289 (this digest's pin).
- ADR-0237 — `vmaf-tune` umbrella spec.
- Netflix Tech Blog — "VMAF: The Journey Continues" (2018-10).
- Apple HLS Authoring Specification (TN2224).
- BBC R&D — Adaptive Bitrate Streaming with x264 / x265 / AV1 study.
- PR #354 audit, Bucket #8.
