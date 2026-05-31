<!-- markdownlint-disable MD060 -->
# ADR-0237: Quality-aware encode automation surface (`vmaf-tune`)

- **Status**: Accepted (Phase A only; Phases B–F remain Proposed)
- **Date**: 2026-05-02 (Proposed); 2026-05-03 (Phase A acceptance)
- **Deciders**: Lusoris
- **Tags**: tooling, ai, ffmpeg, codec, automation, fork-local

## Context

The fork has built a deep quality-measurement stack — VMAF + tiny-AI
fusion regressors (`vmaf_tiny_v2`, `fr_regressor_v1`), no-reference
metrics (`nr_metric_v1`), perceptual extractors (LPIPS,
SSIMULACRA 2, CIEDE2000, CAMBI, psnr_hvs), pre/post filters
(`vmaf_pre`, `learned_filter_v1`), saliency (MobileSal placeholder),
shot detection (TransNet V2 placeholder), and a per-shot CRF
predictor (T6-3b in flight). It also ships first-class FFmpeg
integration (six in-tree patches against `n8.1`, with CPU /
CUDA / Vulkan filters) and a codec-aware FR regressor surface
(ADR-0235 codec collision: `ai/src/vmaf_train/codec.py`, six-bucket
codec one-hot, training BLOCKED on corpus).

What the fork **does not** ship is the action loop: nothing
*drives* the encoder. Every metric we compute is on someone
else's encode. The natural next layer — for an opinionated
fork that already ships every quality input a per-title /
per-shot / per-codec optimiser would need — is a
quality-aware encode automation tool that closes the loop:
given a source and a quality target (or a bitrate budget,
or a Pareto request), drive FFmpeg to find the encoding
parameters that hit it.

The user-facing framing is: *the fork becomes a
"quality + codec parameterisation automation tool"* on top
of the canonical Netflix VMAF reference numbers, with
`vmaf-tune` as the integration point. This ADR pins the
scope and phase ordering before any code lands, since the
implementation surface is large enough to grow unbounded if
not constrained up front.

## Decision

We will ship `tools/vmaf-tune/` as a new fork-local automation
surface. It is a hybrid C + Python tool (same shape as the
existing `vmaf-perShot` binary), built via Meson alongside the
rest of the libvmaf tree. The tool exposes one harness layer
(drive FFmpeg with parameter grids, capture bitrate + decode

- score-via-libvmaf), one search layer (target-quality bisect /
Bayesian / Pareto), and one selector layer (pre-trained per-title
and per-shot CRF predictors with codec-aware conditioning).

The codec scope is multi-codec from day one — `libx264`,
`libx265`, `libsvtav1`, `libvpx-vp9`, `libvvenc`, plus
neural-codec adapters (DCVC family / CompressAI research models /
NVC) and emerging codecs (LCEVC, EVC, AV2 when ffmpeg gains
support) — but each codec is gated behind a thin **codec adapter**
interface so we never special-case the search loop on codec
identity. Phase A ships the harness against `libx264` only;
adapters are added one-per-PR as the underlying corpora exist.

We will land this as an **ADR-Proposed-only** PR for now (no
code), with Research-0044 as the option-space digest. Phase A
implementation lands as a separate PR gated on the user
greenlighting the design + corpus plan in this ADR.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Hybrid C + Python under `tools/vmaf-tune/` (chosen)** | Matches existing fork tools (`vmaf-perShot`, `vmaf_roi`); harness can call libvmaf in-process via the C API for speed; Python wraps FFmpeg + search + AI inference; meson installs alongside `vmaf` | Two-language seam adds complexity; build-system surface grows | Picked per `req` (popup Q3 chose `tools/vmaf-tune/`) and matches the existing pattern; the C-side avoids a process-boundary penalty when the harness scores thousands of encodes |
| Pure Python under `ai/automation/` | Fastest iteration; reuses `ai/`'s `pyproject.toml`; ships as console script | Process-boundary cost on every score (spawn `vmaf` binary per encode); doesn't compose with libvmaf's preallocation API; mixing automation + training under `ai/` muddies the separation | Rejected: the harness is an installable binary, not a research script — wrong tree |
| Top-level `automation/` subtree | Signals "new product surface"; clean for growth into multiple binaries | New top-level dir for one tool initially; no integration with existing meson build | Rejected: premature; revisit if `vmaf-tune` grows multiple sibling binaries |
| MCP-server-only (`mcp-server/vmaf-mcp/` + new tools) | Agent-driven from day one; reuses the JSON-RPC surface | Forces non-agent users through MCP; heavier dep on MCP runtime; not a CLI tool | Rejected: orthogonal — MCP wiring is Phase F, *layered on top of* the CLI tool, not in place of it |
| Defer entirely; encode automation lives downstream | Smaller fork surface; lets others build on libvmaf | Wastes the per-shot / saliency / codec-aware infrastructure that's already 70% of the work; nobody else has the integrated quality stack | Rejected: user explicitly asked for the fork to take this scope (`req`) |
| Single codec at a time, multi-codec deferred | Faster Phase A ship; less corpus burden | Codec adapter interface is harder to retrofit than to design up front; codec-aware FR regressor (ADR-0235) already commits to multi-codec | Rejected: multi-codec is the whole point; ship the adapter interface in Phase A even if only x264 is wired |
| AV1-only (mirror av1an's scope) | Minimum viable; av1an proves the bisect strategy works | x264 has 100× the deployment leverage; encoding x264 is 10–100× faster than AV1 so the corpus generation is cheaper | Rejected: x264 first, AV1 follows |

## Consequences

- **Positive**:
  - The fork's quality stack (every metric + tiny-AI model + per-shot
    infra) finally has a consumer that exercises it end-to-end —
    the strongest dogfood we'll get for the metric pipeline.
  - Closes the loop on the codec-aware FR regressor (ADR-0235
    collision): Phase A's harness produces the corpus that unblocks
    its training.
  - Per-shot CRF predictor (T6-3b) gains a downstream consumer
    that turns its predictions into actual encodes.
  - MCP integration in Phase F makes the whole automation surface
    agent-callable — an agent can "encode this clip at VMAF=93,
    using x265, with a 2-second-GOP constraint" via tool calls.
  - Differentiates the fork against upstream Netflix/vmaf without
    forking the metric definitions (Netflix golden gate stays
    intact; this is a purely additive tool tree).

- **Negative**:
  - New 6-month-class workstream. Phase A alone is ~1 week;
    the full A→F path is ~2–3 months at the user's typical
    cadence. Risk of half-finished phases is real.
  - Encoder version coupling: x264 / x265 / svt-av1 / libvpx /
    libvvenc default values shift between versions. The harness
    has to capture encoder build + commit + version into every
    parquet row, and CI has to pin a known encoder set.
  - Training corpus we own: per-title / per-shot CRF predictors
    need (source, encode, score) tuples. We have Netflix Public
    NViD + BVI-DVC sources; *the encodes have to come from
    Phase A's harness because we can't redistribute third-party
    encodes*. This makes Phase A a hard prerequisite for every
    AI phase.
  - Neural-codec adapters (DCVC, NVC, CompressAI) are research-grade,
    Python-only, depend on heavy ML stacks (PyTorch, CUDA). They
    will live behind an opt-in `vmaf-tune` extra and never block
    the traditional-codec path.

- **Neutral / follow-ups**:
  - Research-0044 ships in the same PR as this ADR — the
    option-space digest covering encode-search strategies (grid /
    coordinate descent / Bayesian / bisect), prior art (av1an,
    ab-av1, Netflix per-title, Bitmovin Per-Title), training-corpus
    plan, and the codec-adapter interface sketch.
  - **Phase A** (encode harness MVP, ~1 week, x264-only) ships
    standalone; the `vmaf-tune` binary plus a Parquet schema for
    the (params, bitrate, metrics) corpus. Useful by itself.
  - **Phase B** (target-VMAF bisect, ~3 days) ports av1an-style
    binary search across our metric set.
  - **Phase C** (per-title CRF predictor, ~1 week, gated on
    Phase A producing corpus) trains a small regressor: source
    canonical-6 + codec one-hot + resolution + framerate → CRF
    for target VMAF.
  - **Phase D** (per-shot dynamic CRF, gated on T6-3b landing)
    consumes per-shot CRF predictions, emits `--qpfile` / x265
    zone files, drives 2-pass encode.
  - **Phase E** (Pareto ABR ladder, ~1 week) per-title across
    resolutions; emits a manifest in DASH/HLS-friendly shape.
  - **Phase F** (MCP tools, ~3 days) `encode_search`,
    `recommend_crf`, `generate_ladder` exposed via vmaf-mcp.
  - New docs surfaces: `docs/usage/vmaf-tune.md`,
    `docs/ai/models/per_title_crf.md`, ffmpeg recipe additions
    in `docs/usage/ffmpeg.md`.
  - Codec adapter interface lives at
    `tools/vmaf-tune/codec_adapters/<codec>.py` — one file per
    codec, must declare its parameter space, its quality knob
    (CRF / CQ / qmin-qmax / λ-control), its 2-pass shape, and
    its log-parsing for emitted bitrate.
  - Test-data licensing audit: any sources we encode for the
    training corpus stay under their original licence; the
    *encodes* are fork-generated and gitignored — only the
    parquet of features + scores ships.
  - This ADR will be split into per-phase ADRs as each phase
    lands — ADR-0237 stays the umbrella; ADR-0237a (Phase A
    harness), 0237b (bisect), etc. will be linked from here.

## References

- Source: `req` 2026-05-02 — *"what if we change the scope to the
  fork that of course netflix is always the vmaf number truth but
  our fork will be a full quality metric and codec parametring
  automation tool/ai? like in combination with ffmpeg of course"*
  (paraphrased per CLAUDE.md user-quote rule).
- Popup `Q1` 2026-05-02: scope = `Just write the spec / RFC for now`.
- Popup `Q2` 2026-05-02: codecs = `x264 + x265 + AV1 + VP9 + that ai
  codec and more modern codecs of course` (translates to the
  multi-codec phasing in this ADR).
- Popup `Q3` 2026-05-02: location = `tools/vmaf-tune/ (C + Python
  mix, like vmaf-perShot)`.
- [Research-0044](../research/0044-quality-aware-encode-automation.md)
  — option-space + prior art + corpus plan + codec-adapter interface.
- [ADR-0235](0235-codec-aware-fr-regressor.md) — codec-aware FR
  regressor v2 (training BLOCKED, corpus produced by Phase A).
- [ADR-0223](0223-transnet-v2-shot-detector.md) — shot
  detection for Phase D.
- T6-3b backlog — per-shot CRF predictor.
- [ADR-0186](0186-vulkan-image-import-impl.md) — Vulkan zero-copy
  import path consumed by `vf_libvmaf_vulkan`; the harness will
  use this when scoring on the encode side to keep CPU↔GPU traffic
  off the hot path.
- Prior art surveyed in Research-0044: av1an, ab-av1, Netflix
  per-title (2015 paper + dynamic optimiser), Bitmovin Per-Title,
  qpfile / x265 zone files / svt-av1 segment table, av1an
  `--target-quality`, ffmpeg-bitrate-stats.
- Bristol VI-Lab 2026 NVC review (`docs/research/0033-bristol-nvc-review-2026.md`)
  — neural codec landscape, informs the neural-codec adapter
  scoping.
