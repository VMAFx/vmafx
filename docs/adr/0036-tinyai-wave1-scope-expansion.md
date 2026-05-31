<!-- markdownlint-disable MD013 MD060 -->
# ADR-0036: Tiny-AI Wave 1 scope expanded beyond D20–D23

- **Status**: Superseded by [ADR-0107](0107-tinyai-wave1-scope-expansion.md)
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, cli, framework, mcp

## Context

ADR-0020 through ADR-0023 locked the four tiny-AI capabilities and their user surfaces at an abstract level, but did not specify which checkpoints ship in Wave 1 or which encoder-side hooks are in scope. The user directed a broader Wave 1: "blow the shit out of the tinyai topic... thats a great function".

## Decision

Wave 1 adds, beyond ADR-0020–0023: **(a)** ship baseline checkpoints for C1/C2/C3 into `model/tiny/` (today empty); **(b)** LPIPS-SqueezeNet (~2.5M) as externally-validated FR baseline, exposed as a new libvmaf feature extractor; **(c)** MobileSal saliency (~2.5M) feeding both a saliency-weighted VMAF variant (scoring side) and a per-CTU QP-offset ROI map (`tools/vmaf-roi`, encoder side); **(d)** TransNet V2 (~1M) shot boundaries + per-shot CRF predictor CLI (`tools/vmaf-perShot`) emitting an encoder-ingestible sidecar. Encoder-side hooks: extend `vmaf_pre` from luma-8bit to 10-bit + chroma; add new `vmaf_post` ffmpeg filter for post-reconstruction NR scoring; FastDVDnet temporal pre-filter (~2.5M, 5-frame window) — deferred if Wave 1 is too wide but still in-scope. Op-allowlist expansion: whitelist `Loop` and `If` with a bounded-iteration guard (reject unbounded `trip_count`, default cap 1024) — unlocks MUSIQ attention, RAFT optical flow, small VLMs. `Scan` stays rejected. MCP surface: new `describe_worst_frames` tool that runs a local VLM (SmolVLM ~256M, Moondream2 1.8B Q4 fallback) on N worst-VMAF-delta frames and returns plain-English artifact descriptions. Full roadmap: `docs/ai/roadmap.md`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Ship baseline C1/C2/C3 checkpoints only | Small scope; bisectable | Does not honor user's "blow it out" directive; encoder-side stays untouched | Rejected |
| Expand Wave 1 (chosen) | Each added model <3M params, shares ORT infra, marginal cost low | Op-allowlist must expand | Rationale matches |

Rationale note: narrow option rejected because (a) user explicitly asked to go wide, (b) every model is tiny and shares infra, (c) encoder-side work needs the same allowlist + tensor-I/O extensions, so splitting across waves duplicates infra changes. Loop/If allowlist expansion is the single largest risk, mitigated by the bounded-iteration guard. `describe_worst_frames` is intentionally last-priority — debugging affordance, not critical scoring path.

## Consequences

- **Positive**: Wave 1 delivers LPIPS / saliency / shot-boundary / NR / encoder-side hooks together.
- **Negative**: larger PR footprint; op-allowlist must admit Loop/If safely.
- **Neutral / follow-ups**: ADR-0039 (runtime op walk), ADR-0040 (multi-input API), ADR-0041 (LPIPS extractor), ADR-0042 (docs rule) all descend from this expansion.

## References

- Source: `req` (user: "blow the shit out of the tinyai topic... thats a great function" + popup answers + "well yeah, thats an adr, I allow it")
- Related ADRs: ADR-0020, ADR-0023, ADR-0039, ADR-0040, ADR-0041, ADR-0042
