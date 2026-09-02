<!-- markdownlint-disable MD013 MD060 -->
# ADR-0023: Tiny-AI user surfaces span CLI, C API, ffmpeg, and training

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, cli, framework

## Context

Tiny-AI is not usable unless every real consumer has an entry point: CLI users, libvmaf C clients, ffmpeg pipelines, and model authors. A subset would leave one of these groups with no supported path.

## Decision

We will ship all four user surfaces: (1) `vmaf` CLI `--tiny-model PATH`, (2) `libvmaf` C API `vmaf_use_tiny_model()` in new `dnn.h` + extended `model.h`, (3) ffmpeg patches — option on `libvmaf` filter + new `vmaf_pre` filter, (4) standalone `vmaf-train` Python CLI.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| C API only | Smallest | CLI and ffmpeg users unsupported | Rejected |
| CLI + C API | Covers most | ffmpeg pipelines excluded | Partial |
| All four (chosen) | Every consumer class reachable | Larger surface | Rationale: not duplicates — C API is the single mechanism; CLI/ffmpeg are thin wrappers; `vmaf-train` is the training surface (separate concern) |

## Consequences

- **Positive**: any consumer has a documented path; tiny-AI is a first-class feature, not a gated experiment.
- **Negative**: four places to keep in sync.
- **Neutral / follow-ups**: ADR-0042 requires docs for each surface in the same PR.

## References

- Source: `Q5.4`
- Related ADRs: ADR-0020, ADR-0022, ADR-0042
