<!-- markdownlint-disable MD013 MD060 -->
# ADR-0618: Content-Aware Classifier for Encoder Routing

- **Status**: Proposed
- **Date**: 2026-05-19
- **Deciders**: lusoris
- **Tags**: `ai`, `planning`, `vmaf-tune`, `dnn`

## Context

Different content types require different encoding strategies: animation 2D
compresses well at low resolution and benefits from `tune=animation`; sports
needs high CRF headroom and tight shot boundaries; HDR requires HDR-aware VMAF
models. Currently `vmaf-tune` applies uniform parameters regardless of content
type. A pre-encoding classifier that tags the source once (single 10s clip
analysis) and emits a tag dict enables automatic routing to per-content-type
VMAF priors, encoder parameters, and ladder defaults — without operator
intervention.

## Decision

We will implement a Hybrid A4 classifier (Research 0614): run FFmpeg `siti`
for spatial complexity and motion intensity (deterministic, no GPU, ~5–10 s
per clip), infer dynamic range from container metadata, and use CAMBI as a
source-quality proxy. For genre and subjective tags (the only tags that cannot
be derived from signal statistics), run a single local VLM call via Ollama
(Gemma 3B Vision or Llama 3.2 11B Vision) on 3 sampled frames. Results are
cached per clip fingerprint (xxHash128 of the first 1 MB) in a sidecar JSON.

The classifier lives in `tools/vmaf-tune/src/vmaftune/classify.py`. The output
is a `ContentTags` dataclass fed into a routing table that selects encoder
params, VMAF model, and ladder priors.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| A1 — VLM-only (Ollama local) | Zero training; all tags covered | GPU required; 2–5 s latency; non-deterministic | Retained as partial contributor in A4 |
| A1b — Claude Vision API | Zero training; natural language | Content leaves premises; API cost; latency | Unacceptable for unreleased content |
| A2 — Train small CNN | Zero runtime overhead; deterministic | 1–2 weeks training; taxonomy crosswalk; genre-only | Deferred; requires labeled corpus |
| A3 — CAMBI + SI/TI only | Deterministic; fast; no GPU | No genre; no semantic tags | Covers 5 of 7 tag categories; adopted as A4 base |
| A4 — Hybrid SI/TI + selective VLM (chosen) | Covers all tags; deterministic for signal tags; VLM only when needed | VLM non-determinism on genre tags; Ollama dependency | — |

## Consequences

- **Positive**: Zero training burden; all 7 tag categories covered; enables
  automatic routing for NEG (ADR-0616), ladder priors, and encoder `tune`
  params; caching means the classifier runs once per title.
- **Negative**: Ollama dependency adds GPU requirement to the pre-encoding
  step when genre tags are needed; non-deterministic genre tags (LLM-sampled)
  may produce inconsistent routing on re-runs without a fixed seed.
- **Neutral / follow-ups**: A deterministic fallback (A3-only, no genre) must
  be available when Ollama is absent (CI environments, CPU-only runners).
  CNN classifier (A2) is the long-term upgrade path once a labeled corpus is
  assembled.

## Dependencies

- **ADR-0616** (VMAF NEG) — the classifier's `animation-3d` tag auto-selects
  NEG; ADR-0616 must land first.
- No other items in the 6-list are hard prerequisites; classifier can ship
  without DO, ABR rendition, or cross-shot weighting.
- FFmpeg `siti` filter available on the dev machine (confirmed in-container).
- CAMBI available via libvmaf CLI (in-tree).
- Ollama with Gemma 3B or Llama 3.2 11B Vision available on dev machine.

## Implementation phases

| Phase | Description | Effort |
|-------|-------------|--------|
| P1 | `classify.py` skeleton; `ContentTags` dataclass; FFmpeg SI/TI extraction | 2 days |
| P2 | CAMBI source-quality proxy; container metadata HDR detection | 1 day |
| P3 | Ollama VLM genre tagger; fallback to `genre=unknown` when Ollama absent | 2 days |
| P4 | Routing table: `ContentTags` → encoder params + VMAF model + ladder priors | 1 day |
| P5 | Cache (xxHash128 fingerprint + sidecar JSON); CLI `--classify` subcommand | 1 day |
| P6 | Docs `docs/usage/vmaf-tune-classifier.md`; routing table docs | 1 day |

Total estimate: 8 days (largest item on the roadmap).

## References

- Research digest: [docs/research/0614-content-aware-classifier-research.md](../research/0614-content-aware-classifier-research.md).
- FFmpeg `siti` filter: ITU-T P.910 SI/TI.
- CAMBI: `resource/doc/cambi.md` (Netflix/vmaf upstream; retrieved via GitHub API 2026-05-19).
- MediaPipe Video Classification (mediapipe.dev; 2023).
- Anthropic Claude Vision API (claude.ai/docs; 2025).
- `tools/vmaf-tune/src/vmaftune/saliency.py` — existing visual-feature pipeline.
- Source: per user direction (roadmap planning session 2026-05-19).
