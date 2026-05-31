<!-- markdownlint-disable MD013 MD060 -->
# ADR-0018: Claude hooks scope includes safety and auto-format

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: claude, agents, ci, git

## Context

Hooks are the harness-level enforcement layer for behaviours Claude itself cannot guarantee (the harness executes them, not the model). Narrow hooks would leave safety gaps; wider coverage means every session gets the same guardrails regardless of model slip.

## Decision

We will implement hooks for: PreToolUse safety (block force-push etc.) + PostToolUse auto-format + snapshot-warn + compile-commands sync + git `pre-commit`/`commit-msg`/`pre-push`/`post-checkout`/`post-merge` + session-start/stop.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Commit/push hooks only | Minimal | Session-level and safety hooks unused | Gaps too large |
| Full hook set (chosen) | Every event has its enforcer | More config to maintain | User explicitly requested |

This decision was a default — user requested the breadth.

## Consequences

- **Positive**: force-push blocked at tool level; auto-format guaranteed; snapshot changes surface warnings.
- **Negative**: PreToolUse can occasionally block legitimate actions (escape hatch is explicit override).
- **Neutral / follow-ups**: ADR-0035 corrects the hooks schema shape.

## References

- Source: `req` (user: "claude skills and hooks need more, like adding a new gpu backend or whatever")
- Related ADRs: ADR-0017, ADR-0035
