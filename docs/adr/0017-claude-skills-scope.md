<!-- markdownlint-disable MD013 MD060 -->
# ADR-0017: Claude skills scope includes domain scaffolding

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: claude, agents, framework

## Context

A minimal Claude-skill surface (build, test, lint) would not capture the fork's actual workflows. The user requested skills that scaffold new surfaces ("claude skills and hooks need more, like adding a new gpu backend or whatever"). Skills are the canonical entry point for repeatable multi-step tasks.

## Decision

We will scope Claude skills to cover: build + test + domain scaffolding (`add-gpu-backend`, `add-simd-path`, `add-feature-extractor`, `add-model`) + profiling + bisect + upstream port/sync + release + format/lint.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Build/test/lint only | Tiny skill set | Misses the scaffold workflows that motivated the fork | Rejected per user request |
| Full workflow coverage (chosen) | One canonical entry per repeated task | Larger skill inventory to maintain | User explicitly requested |

This decision was a default — the user specified the breadth.

## Consequences

- **Positive**: every common fork workflow has a named skill; agents (human or AI) have predictable commands.
- **Negative**: skill set must be maintained and kept in sync with real flows.
- **Neutral / follow-ups**: ADR-0018 complements this with hooks.

## References

- Source: `req` (user: "claude skills and hooks need more, like adding a new gpu backend or whatever")
- Related ADRs: ADR-0018
