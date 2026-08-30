<!-- markdownlint-disable MD013 MD060 -->
# ADR-0009: MCP server exposes four core tools

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: mcp, python, framework

## Context

The fork ships a Model Context Protocol server under `mcp-server/vmaf-mcp/`. A focused tool surface is better than a sprawling one: every tool is a JSON-RPC method that agents can discover, and each adds review and test burden. The planning popup offered a curated set rather than "everything the CLI can do".

## Decision

We will expose four MCP tools: `vmaf_score`, `list_models`, `list_backends` (SIMD caps + GPU devices), and `run_benchmark`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Single `vmaf` catch-all tool | Minimal surface | Agents have to synthesize subcommand syntax; bad UX | Tool discovery is the whole point of MCP |
| Mirror every CLI flag as a tool | Exhaustive | Huge surface; many overlap; review burden | Diminishing returns after the core four |

This decision was a default — the alternatives were broad/narrow extremes that the focused four-tool answer displaced.

## Consequences

- **Positive**: agents discover four orthogonal capabilities; each has a tight JSON schema.
- **Negative**: any new capability requires a new tool entry and schema update.
- **Neutral / follow-ups**: ADR-0042 adds `describe_worst_frames` (tiny-AI surface).

## References

- Source: `Q3.3`
- Related ADRs: ADR-0042
