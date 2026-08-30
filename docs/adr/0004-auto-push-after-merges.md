<!-- markdownlint-disable MD013 MD060 -->
# ADR-0004: Auto-push sycl and master to origin after merges

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: git, ci, release

## Context

After the `gpu-opt → sycl → master` merge path completes locally, the branches must reach `origin` promptly so CI and downstream collaborators see the same state. A manual push step is easy to forget.

## Decision

We will auto-push `sycl` and `master` to `origin` after merges complete during the planning-driven integration phase, so remote and local state stay in sync without a separate manual step.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Manual push after every merge | Explicit control | Easy to forget; leaves remote stale | Automation wins for a predictable flow |
| Never auto-push | Maximum caution | Defeats the purpose of CI as fast feedback | Rejected — we want CI to fire immediately |

This decision was a default — no alternatives were weighed beyond the minimal option above.

## Consequences

- **Positive**: CI fires immediately on integration; collaborators are not blocked on a missing push.
- **Negative**: no pre-push pause to reconsider; depends on pre-push hook (`make lint`) discipline.
- **Neutral / follow-ups**: ADR-0037 branch protection catches any push that bypassed local checks.

## References

- Source: `Q1.4`
- Related ADRs: ADR-0002, ADR-0037
