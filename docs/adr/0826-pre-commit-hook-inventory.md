# ADR-0826: pre-commit hook inventory and `forbid-new-submodules` addition

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `ci`, `build`, `security`, `hygiene`, `fork-local`

## Context

The project's `.pre-commit-config.yaml` had grown organically. A coverage audit
against a standard checklist of hygiene hooks (conflict markers, whitespace, line
endings, key detection, YAML/JSON/TOML syntax, shell tooling, C formatting, Python
formatting, secret scanning) revealed that all items except `forbid-new-submodules`
were already wired. `forbid-new-submodules` was the single missing hook.

Additionally, CONTRIBUTING.md contained no structured inventory of the hooks that
fire on `git commit` and `git push`. New contributors had no single place to see
what runs automatically and why, leading to surprise failures and manual searches
through the YAML.

## Decision

1. Add `forbid-new-submodules` (from `pre-commit/pre-commit-hooks`) to the
   `pre-commit-hooks` block in `.pre-commit-config.yaml`. No exclude patterns are
   needed: the hook fires only when a `.gitmodules` file or `[submodule]` section
   is added, which should never occur in this repo.

2. Add a hook-inventory table to CONTRIBUTING.md (between the Quickstart section
   and the Core rules section) listing every hook, its source repo, its stage
   (commit vs. pre-push), and a one-line purpose description.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| Keep CONTRIBUTING.md as-is, rely on readers inspecting the YAML | No churn | YAML is dense; contributors have to parse rev pins and exclude regexes to understand intent | Documentation is a first-class requirement per ADR-0100 |
| Add a `check-gitmodules` script instead of the upstream hook | Full control | Duplicates well-tested upstream logic | Upstream hook is the canonical implementation; no reason to reinvent |
| Skip `forbid-new-submodules` (low probability of accidental submodule addition) | No change | A merge conflict resolution or copy-paste from another repo can silently add a submodule pointer | Low-cost hook; the asymmetry between cost and protection value makes it worth adding |

## Consequences

- **Positive**: `forbid-new-submodules` blocks the entire class of accidental submodule
  introductions at commit time. CONTRIBUTING.md now provides a complete, scannable hook
  inventory so contributors understand what fires and why before they push.
- **Negative**: negligible — `forbid-new-submodules` adds one fast filesystem check per
  commit; CONTRIBUTING.md grows by ~35 lines.
- **Neutral / follow-ups**: the inventory table must be updated whenever hooks are added
  or removed from `.pre-commit-config.yaml`.

## References

- Audit request: per user direction (2026-05-29).
- Related: [ADR-0105](0105-copyright-header-check.md) (check-copyright),
  [ADR-0332](0332-agent-worktree-drift-guard.md) (agent-worktree-drift-guard),
  [ADR-0386](0386-adr-numbering-collision-prevention.md) (check-adr-numbering),
  [ADR-0108](0108-deep-dive-deliverables-rule.md) (validate-pr-body).
