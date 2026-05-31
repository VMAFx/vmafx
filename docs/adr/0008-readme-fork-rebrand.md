<!-- markdownlint-disable MD013 MD060 -->
# ADR-0008: Rewrite README with fork branding preserving Netflix attribution

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: docs, readme, license

## Context

The upstream README is written from Netflix's perspective and does not surface fork-added capabilities (SYCL/CUDA/HIP backends, tiny-AI, MCP server, precision flag). A casual reader landing on the fork's repo page must immediately see what is different and how to build it, while the Netflix authorship and license must remain visible.

## Decision

We will rewrite the README with fork-first branding, preserve Netflix attribution and the BSD-3-Clause-Plus-Patent license notice, and add the Ko-fi handle `lusoris` (kofi.com/lusoris).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep upstream README verbatim | Zero divergence from upstream | Fork capabilities invisible to newcomers | Defeats the point of having a fork |
| Prepend a fork banner, keep rest | Minimal diff | Mixed voice; fork additions still buried | User asked for a proper rewrite |

This decision was a default — the narrow alternative of a "banner-only" readme was considered but rejected as insufficient to showcase fork capabilities.

## Consequences

- **Positive**: new users see backends, tiny-AI, MCP server immediately; attribution and license stay visible.
- **Negative**: divergence from upstream README makes mechanical sync impossible.
- **Neutral / follow-ups**: section headings mirror golusoris README for consistency.

## References

- Source: `Q2.4`, `Q3.2`
- Related ADRs: ADR-0025
