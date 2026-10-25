<!-- markdownlint-disable MD013 MD060 -->
# ADR-3374: Content-Aware Classifier with Fail-by-Name Vision Backend

- **Status**: Proposed
- **Date**: 2026-10-25
- **Deciders**: lusoris
- **Tags**: `vmaf-tune`, `ai`, `planning`

## Context

[ADR-0618](0618-content-aware-classifier.md) outlines a hybrid pre-encoding
classifier for `vmaf-tune` that extracts spatial complexity (SI) and temporal
motion (TI) via the ITU-T P.910 filter in FFmpeg, probes container geometry and
dynamic range (HDR/SDR), and suggests invoking a vision-language model (VLM)
for genre tagging.

However, ADR-0618 specified a fallback to `genre=unknown` whenever the vision
backend was absent. Silent fallback to `unknown` masks configuration defects,
makes test environments diverge silently from production runs, and risks
applying default encoder tuning parameters to content that required specialized
handling.

Furthermore, video frames must never be dispatched to hosted cloud services
where unreleased visual media could leak. Video frames must remain strictly on
local premises (loopback or RFC 1918 private subnets).

Finally, ADR-0618 proposed a cache key based on the first 1 MB of the source
file. As demonstrated in [ADR-3372](3372-perceptual-video-fingerprint.md), a 1 MB
prefix key causes false collisions between clips sharing intro sequences or
idents. The classifier requires robust caching aligned with perceptual video
fingerprinting when available, backed by full content hashing.

## Decision

1. **Phases P1 and P2 shipped.** `vmaftune.classify` implements the `ContentTags`
   frozen dataclass, ITU-T P.910 SI/TI extraction via FFmpeg `siti` with mean
   and 95th percentile metrics, container geometry probing, dynamic range
   detection via `vmaftune.hdr.detect_hdr`, and motion/detail stratification
   into named classes (`static`, `low`, `high`, `very_high`).
2. **Fail-by-name on missing vision backend (supersedes ADR-0618 fallback).**
   When genre classification is requested (`genre=True`), the classifier invokes
   the configured VLM backend. If no VLM backend is provided or the endpoint
   lacks vision capabilities, `classify` raises `VisionBackendUnavailable`,
   naming the missing backend and the configuration key (`--vlm-url` or `vlm`).
   Silent coercion to `unknown` is strictly prohibited.
3. **Local-only frame transport.** The `OpenAICompatVlm` implementation sends
   sampled frames to a `/v1/chat/completions` endpoint. It validates the target
   hostname and refuses any non-loopback and non-RFC1918 host by default
   (`allow_remote=False`). Video frames never leave the local environment for
   hosted lanes.
4. **Capability probe.** Endpoints are probed once for image input capability.
   Any HTTP 4xx error or message indicating unsupported image input raises
   `VisionBackendUnavailable`.
5. **Exact genre vocabulary.** The allowed genre labels are exactly:
   `("movie", "tv", "anime", "adult", "sports", "unknown")`. Lawful adult
   content is recognized as a standard category rather than excluded or coerced.
   Responses outside this vocabulary raise a validation error instead of being
   coerced.
6. **Sidecar cache resilience.** Classification results are cached in a sidecar
   JSON keyed by clip fingerprint and classifier version. When `fingerprint.py`
   perceptual fingerprinting is available and the clip satisfies the 30-frame
   requirement, the perceptual fingerprint is reused; otherwise, exact content
   hashing (SHA-256) is used. Corrupt sidecar JSON is discarded with a logged
   warning naming the corrupted file, and the analysis is recomputed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fail-by-name vision backend (chosen) | Explicit failure mode; prevents accidental misclassification and silent parameter drift | Requires configured endpoint when genre is enabled | Enforces operational correctness and contract integrity |
| Silent fallback to `unknown` (ADR-0618) | Does not fail in environments without VLM | Masks misconfigured pipelines; gives false sense of classification; non-auditable | Defect hiding; superseded |
| Hosted Vision API | Zero local GPU requirement | Video frames leave premises; latency; API billing | Violates privacy invariant for unreleased media |
| First 1 MB xxHash128 cache key | Instant key computation | Collides on identical openings; misses on container changes | Rejected by ADR-3372 findings |

## Consequences

- **Positive**: Pre-encoding classifier provides deterministic SI/TI signal
  metrics, HDR detection, and explicit genre classification. Misconfigurations
  fail immediately by name. Frames remain securely on local networks.
- **Negative**: Operators requesting genre tagging must provide an accessible
  local VLM endpoint supporting vision chat completions.
- **Neutral / follow-ups**: Encoder routing table (Phase P4) can now consume
  `ContentTags` to automatically select ladder priors, VMAF models, and encoder
  tuning parameters.

## References

- Operator decision via controller ledger ids `vmafx-near-dup-genre-20261024`
  and `vmafx-genre-vision-lane-20261025` (paraphrased).
- [ADR-0618](0618-content-aware-classifier.md): Content-Aware Classifier for
  Encoder Routing.
- [ADR-3372](3372-perceptual-video-fingerprint.md): Perceptual video
  fingerprint for near-duplicate result reuse.
- ITU-T Recommendation P.910 (07/2008), Annex B.
