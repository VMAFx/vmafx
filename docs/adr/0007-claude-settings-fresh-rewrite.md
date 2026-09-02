<!-- markdownlint-disable MD013 MD060 -->
# ADR-0007: Rewrite .claude/settings.json from scratch

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: claude, agents

## Context

The existing `.claude/settings.json` held an ad-hoc allowlist accreted over prior sessions. Carrying it forward would mean auditing every entry and reconciling with the new hook and skill scopes (ADR-0017, ADR-0018). A clean rewrite aligned the file with the current Claude Code schema (ADR-0035) and the fork's expanded skill set.

## Decision

We will rewrite `.claude/settings.json` fresh, discarding the existing allowlist, and re-derive permissions, hooks, and environment variables from the current plan.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep + extend existing allowlist | Preserves historical permissions | Hidden entries with unknown provenance; schema mismatch with ADR-0035 | Audit cost exceeds rewrite cost |
| Fresh rewrite (chosen) | Clean baseline; matches new hook/skill scope | Temporary loss of ambient permissions until rediscovered | User explicitly chose this |

This decision was a default alternative was "keep existing"; rejected because its entries were unaudited.

## Consequences

- **Positive**: every permission in the file has a known reason; schema matches Claude Code current expectations.
- **Negative**: transient permission prompts as the new allowlist catches up.
- **Neutral / follow-ups**: ADR-0035 fixes the hooks schema.

## References

- Source: `Q2.3`
- Related ADRs: ADR-0017, ADR-0018, ADR-0035
