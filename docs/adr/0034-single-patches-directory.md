<!-- markdownlint-disable MD013 MD060 -->
# ADR-0034: Delete patches/ leftover, keep only ffmpeg-patches/

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: workspace, build

## Context

The tree had two competing patch locations: `ROOT/patches/` (a single bare `ffmpeg-libvmaf-sycl.patch` diff) and `ffmpeg-patches/` (proper `git format-patch` files). Two locations for the same concept confuses readers and invites drift.

## Decision

Delete `ROOT/patches/`. Canonical location is `ffmpeg-patches/` with proper `git format-patch` files (`0001-libvmaf-add-tiny-model-option.patch`, `0002-add-vmaf_pre-filter.patch`, `0003-libvmaf-wire-sycl-backend-selector.patch`). Dockerfile now copies `ffmpeg-patches/0003-libvmaf-wire-sycl-backend-selector.patch` instead of the legacy bare diff.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep both | Zero removal risk | Two sources of truth; drift certainty | Rejected |
| Merge `patches/` into `ffmpeg-patches/` | Preserves content | Duplicates the SYCL diff | The diff was already superseded |
| Delete `patches/` (chosen) | Single canonical location | Removes a file | Correct; content was redundant |

## Consequences

- **Positive**: one canonical patch location; Dockerfile points at the current series.
- **Negative**: git blame for the deleted diff requires history dive.
- **Neutral / follow-ups**: `/refresh-ffmpeg-patches` skill maintains the canonical series.

## References

- Source: `req` (user: "some project rood dirs should be cleaned up/moved as well")
- Related ADRs: ADR-0029
