<!-- markdownlint-disable MD013 MD060 -->
# ADR-0005: Adopt full framework adaptation scope a-g

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: framework, ci, docs, build, mcp

## Context

The planning round offered a choice between a narrow subset of framework adaptations and the full program (AI onboarding, lint/CI, supply chain, README, principles, Makefile, MCP server). A narrower scope would ship faster but leave gaps that later decisions would have to backfill.

## Decision

We will implement the full framework adaptation program, items (a) through (g): AI onboarding, lint/CI, supply chain, README, principles, Makefile, and MCP server.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Subset (e.g. just CI + README) | Faster initial landing | Leaves half-integrated surfaces; principles and Makefile deltas drift | The sub-decisions (ADR-0007 through ADR-0018) all assume the full scope |
| Full program a-g | One coherent adoption | More work up-front | Chosen — user picked the comprehensive option |

This decision was a default in the sense that the user selected the widest option; the narrower options were on the popup but rejected.

## Consequences

- **Positive**: every downstream sub-decision (skills, hooks, coding standards, MCP surface) rests on a consistent foundation.
- **Negative**: large surface to implement and maintain.
- **Neutral / follow-ups**: ADRs 7, 9, 10, 11, 12, 17, 18 flesh out sub-scope.

## References

- Source: `Q2.1`
- Related ADRs: ADR-0007, ADR-0009, ADR-0010, ADR-0011, ADR-0012, ADR-0017, ADR-0018
