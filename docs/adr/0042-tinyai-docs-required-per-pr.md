<!-- markdownlint-disable MD013 MD060 -->
# ADR-0042: Tiny-AI PRs must ship human-readable docs in the same PR

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, docs

## Context

Tiny-AI is net-new fork content with no upstream docs to lean on. Each model has different input conventions (YUV bit-depth, colour range, resolution bounds) and different output semantics (distance vs. saliency mask vs. shot-boundary probability vs. denoised frame). "Figure it out from the code" is an unreasonable ask for external users. The `docs/ai/` tree already had `inference.md` and `training.md` at a system level but no per-model pages for the Wave 1 checkpoints (LPIPS-Sq, MobileSal, TransNet V2, FastDVDnet). User: "all those ai parts we create need especially good documentation in docs with examples how to use it etc... this is a topic where the doc side for humans needs a bit substance".

## Decision

Any change to `ai/`, `core/src/dnn/`, DNN-backed feature extractors in `core/src/feature/`, `model/tiny/`, `mcp-server/`, or any user surface wired to a tiny model (`--tiny-model`, `vmaf_pre`, `vmaf_post`, `vmaf-train`, `describe_worst_frames`) must ship corresponding docs under `docs/ai/` in the same PR. Minimum bar per change: (a) plain-English description of what the model/feature does; (b) output range + interpretation (e.g. "LPIPS score: 0=identical, ~1=maximally dissimilar"); (c) runnable copy-pasteable usage example (CLI invocation, C API snippet, or Python call); (d) provenance of any shipped `.onnx` checkpoint (upstream source, license, pinned sha256); (e) known limitations (bit-depth, colour space, min resolution, CPU vs GPU path). Enforced by CLAUDE.md §12 rule 10 + AGENTS.md §12 rule 7; PR review gate checks for a `docs/ai/` diff when any tiny-AI path changes.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Rely on code comments + ADR rows | Zero new rule | Both are maintainer-facing; don't give external users a copy-pasteable entry point | Rejected |
| Ship docs in follow-up PR | Lower per-PR friction | Splits mental model from artefact; routinely slips (default failure mode of "docs debt") | Rejected |
| Restrict to `docs/ai/inference.md` only | One file to update | Each model has distinct usage surfaces; index navigation gets deep | Rejected |
| Docs in same PR, per-model page required (chosen) | User can go README → `docs/ai/` → working invocation without reading source | Per-PR cost | Correct |

Rationale note: treating "document new features" as implicit has already failed once — Wave 1 queue landed without per-model pages. Each model's distinct input/output contract makes per-model pages the right granularity. Cost is small (~1 page markdown + snippet); benefit is an external user can self-serve. Scoped deliberately to tiny-AI rather than whole codebase because (a) classic VMAF is already documented upstream, (b) tiny-AI is net-new fork content.

## Consequences

- **Positive**: every tiny-AI PR ships usable docs; the doc side has substance; external users have a path.
- **Negative**: per-PR cost — each new extractor/model needs a docs commit in the same PR.
- **Neutral / follow-ups**: PR review gate enforces the presence of a `docs/ai/` diff for tiny-AI paths.

## References

- Source: `req` (user: "all those ai parts we create need especially good documentation in docs with examples how to use it etc... this is a topic where the doc side for humans needs a bit substance")
- Related ADRs: ADR-0036, ADR-0040, ADR-0041

### Status update 2026-05-08: OpenVINO EP enabled for explicit NPU / CPU / GPU device-type selection

[ADR-0332](0405-openvino-npu-ep-wiring.md) extended the tiny-AI
dispatch surface with three new `--tiny-device` keywords —
`openvino-npu` / `openvino-cpu` / `openvino-gpu` — pinning the
OpenVINO EP to a single `device_type` with no fallback. The NPU
selector targets the Intel AI-PC neural processing unit on Meteor /
Lunar / Arrow Lake silicon ([Research-0031](../research/0031-intel-ai-pc-applicability.md)
follow-up; superseded the earlier DEFER verdict via research-0086
GO recommendation). Per-model docs under `docs/ai/inference.md` were
updated in the same PR to satisfy this ADR's per-PR doc bar; the EP
matrix gained three rows and the `attached_ep` stable-string list
gained `"OpenVINO:NPU"`.
