<!-- markdownlint-disable MD013 MD060 -->
# ADR-0035: Migrate .claude/settings.json hooks to current schema

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: claude, agents

## Context

Claude Code surfaced a hard parse error ("Expected array, but received undefined") on `.claude/settings.json`: the fork used the legacy flat `{matcher, command}` shape while Claude Code's current schema requires the nested `{matcher, hooks: [{type: "command", command}]}` shape. With the old shape, the IDE refused to parse settings, silently dropping the permission allowlist and every registered hook. User: "and fix this please" + settings parse error screenshot.

## Decision

Migrate the `hooks` block in `.claude/settings.json` from `{matcher, command}` to `{matcher, hooks: [{type: "command", command}]}` (current Claude Code schema) so the IDE parses it successfully.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Ship broken settings | No work | Silently drops every hook and the allowlist | Unacceptable |
| Downgrade Claude Code | Keeps old schema | Stops getting fixes | Unacceptable |
| Migrate to current schema (chosen) | Works | None | Forced choice |

Rationale: no real alternative — either update to the current schema or ship a broken settings file.

## Consequences

- **Positive**: hooks execute again; allowlist applies.
- **Negative**: any shared settings snippet written for the old schema must be ported.
- **Neutral / follow-ups**: commit `1b1685e4` landed the fix.

## References

- Source: `req` (user: "and fix this please" + settings parse error screenshot)
- Related ADRs: ADR-0007, ADR-0018
