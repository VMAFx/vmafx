<!-- markdownlint-disable MD060 -->
# ADR-0684: Pre-rebase worktree-drift guard

- **Status**: Accepted
- **Date**: 2026-05-22
- **Deciders**: lusoris, Claude (Opus 4.7)
- **Tags**: agents, ci, git-hooks, fork-local

## Context

[ADR-0332](0332-agent-worktree-drift-hard-guard.md) added a
pre-commit-level guard (`scripts/ci/check-agent-worktree-drift.sh`)
that refuses commits coming from the *main* checkout while one or
more agent worktrees under `.claude/worktrees/agent-*` exist. That
guard works only at commit time. In a 2026-05-22 incident, a
background train-shepherd agent drifted into the main checkout and
ran a sequence of `git checkout` and `git rebase origin/master`
commands targeting PR #1486 — well before any `git commit` attempt
would have triggered ADR-0332. By the time the user noticed (their
open Zed buffer for `.zed/settings.json` showed the file reverted to
its pre-fixup state), the working tree carried 200+ files in mixed
state across two branches plus an in-progress rebase and a
`tmp/1486-cherry` temp branch the agent had created. Recovery
required stashing the agent's mess and resetting to master; no work
was lost because the user happened to look at the file in time.

This is a different drift window than the one ADR-0332 covers:
ADR-0332 catches a commit *after* the working tree has already been
mutated; the 2026-05-22 incident shows the working-tree mutation
itself is the failure that matters when a human is interactively
using the same checkout in an editor.

`git` exposes a real `pre-rebase` hook
([git-hook documentation](https://git-scm.com/docs/githooks#_pre_rebase))
that fires before `git rebase` mutates any tree state. A non-zero
exit from the hook aborts the rebase. The same allow/refuse logic
that ADR-0332 uses on the pre-commit hook applies cleanly: allow if
the rebase's `git rev-parse --show-toplevel` matches
`*/.claude/worktrees/agent-*`, refuse otherwise when at least one
agent worktree is present.

## Decision

Add `scripts/git-hooks/pre-rebase` mirroring the allow/refuse logic
of `scripts/ci/check-agent-worktree-drift.sh`. Install it through
the existing `make hooks-install` Makefile target (extended to loop
over both `pre-push` and `pre-rebase`). Bypass is the standard
`git rebase --no-verify` escape hatch for the human user's
legitimate main-checkout rebases while an agent is running.

`docs/development/agent-worktree-discipline.md` gains a Layer-2b
section describing the new hook alongside the existing pre-commit
guard. `AGENTS.md` is unchanged because the agent-side discipline
(stay in `$AGENT_WT`, use `git -C "$AGENT_WT"`) was already correct
in ADR-0332's documentation; this ADR is purely a host-side
additive.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Pre-rebase hook (chosen)** | Catches the exact failure mode at the earliest mutation point; uses a real git hook (not pseudo); same bypass UX as the existing pre-commit guard; symmetrical implementation. | Adds one more hook to install via `make hooks-install`. | Highest signal-to-noise; matches the existing layer's design verbatim. |
| Wrap `git` in a shell function | Catches checkout, rebase, reset, merge — every tree-mutating operation, not just rebase. | Per-shell, per-user; doesn't survive a fresh agent process group; agents commonly invoke `/usr/bin/git` directly; impossible to ship as a tracked repo artifact. | Brittle, easily bypassed, doesn't compose with the existing layer's hook-based design. |
| `post-checkout` notification hook | Visibility into branch switches; can log to a tracked drift-incident file for forensics. | Cannot refuse (the checkout has already happened by the time post-checkout fires); doesn't address the root failure (the mutation). | Soft warning is less valuable than a hard refusal at the earlier point. May be added as a follow-up if forensic logging becomes useful. |
| Stronger ADR-0332 — refuse not just commits but also `git update-ref` / `git reset` through a single `update` hook | One hook covers many mutation paths. | The `update` hook is server-side (push receiver), not client-side; doesn't run on the developer's machine for local operations. | Wrong hook class; mismatches the use case. |
| Server-side branch-protection rule | Defence in depth at GitHub. | No detection signal for "this push came from a drifted main checkout"; only catches pushes, not local rebase mutation. | Cannot detect the failure pattern. |
| Refuse *all* main-checkout git mutations while *any* worktree exists, no bypass | Strongest invariant. | Blocks every release-please rebase, every legitimate human rebase of master onto upstream/master, every doc-only push. | Too aggressive; same false-positive class ADR-0332 already rejected for commits. |

The pre-rebase hook is the smallest, most-aligned addition that
catches the observed 2026-05-22 incident without expanding scope
beyond the pre-commit guard's design.

## Consequences

- **Positive**: Rebases issued from the main checkout while agent
  worktrees exist are stopped *before* any tree mutation. The user's
  open editor buffers are not silently invalidated. Recovery from a
  rejected rebase is trivial (`git rebase --no-verify` if intended;
  re-issue from inside the worktree if drift). The defence-in-depth
  stack now has three layers — Layer 1 (agent-side discipline,
  `feedback_agents_isolated_worktree_only`), Layer 2a (pre-commit,
  ADR-0332), Layer 2b (pre-rebase, this ADR).
- **Negative**: `make hooks-install` now installs two symlinks
  (`pre-push` + `pre-rebase`) where it previously installed one.
  Human users rebasing their own main-checkout work while an agent is
  active must remember `git rebase --no-verify` (same flag, same UX
  as the existing pre-commit bypass). A teammate who never runs
  `make hooks-install` is not protected — same caveat as the existing
  `pre-push` hook.
- **Neutral / follow-ups**:
  - A post-checkout *notification* hook (warn-only, no refusal) may
    be added later if forensic logging of branch switches under
    active worktrees becomes useful.
  - The next refresh of `docs/development/agent-worktree-discipline.md`
    consolidates Layer 2a + Layer 2b into one section so contributors
    see both hooks side by side.
  - No CI gate is added — these are local hooks; CI cannot enforce
    "the developer's local rebase didn't drift" and shouldn't try.

## References

- Source: `req` — 2026-05-22 incident, user message "well i restarted
  zed again" followed by transcript showing 200-file working-tree
  regression after Agent C ran `git rebase origin/master` from the
  main checkout. Reflog excerpt:
  `8230f3e386 HEAD@{8}: checkout: moving from master to fix/modernization-audit-contract-noise-20260521`
  → `3f4f28a5a8 HEAD@{7}: rebase (start): checkout origin/master`.
- [ADR-0332](0332-agent-worktree-drift-hard-guard.md) — the
  pre-commit-level companion guard.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — fork-local PR
  deep-dive deliverables.
- [ADR-0221](0221-changelog-adr-fragment-pattern.md) —
  fragment-based CHANGELOG / ADR-index pattern.
- Global memory `feedback_agents_isolated_worktree_only` —
  process-side Layer 1 complement.
- `docs/development/agent-worktree-discipline.md` — user-facing
  discipline doc, updated by this PR.
- `git` documentation: <https://git-scm.com/docs/githooks#_pre_rebase>
  (retrieved 2026-05-22).
