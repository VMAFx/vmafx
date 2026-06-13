<!-- markdownlint-disable MD060 -->
# ADR-0332: Agent worktree-drift hard guard

- **Status**: Accepted
- **Status update 2026-05-15**: implemented;
  `scripts/ci/check-agent-worktree-drift.sh` present; wired into
  `.pre-commit-config.yaml`; landed in commit 1141faa15.
- **Date**: 2026-05-09
- **Deciders**: lusoris, Claude (agent kit)
- **Tags**: agents, ci, build, fork-local

## Context

Background coding agents on this fork run in dedicated git worktrees
under `.claude/worktrees/agent-<id>/`. The agent kit is supposed to
keep each agent isolated from the main checkout and from sibling
agents. In practice the agent's process repeatedly ended up running
with `cwd` inside the *main* checkout instead of its assigned
worktree — the "drift" pattern. Five incidents in the 2026-05-09
session lost work or required cherry-pick recovery: PR #498
(AdaptiveCpp), PR #520 (T3-15), PR #526 (ccache), the first attempt
at the MCP runtime v2 PR, and the multi-corpus run.

The user has the global memory rule
`feedback_agents_isolated_worktree_only` ("never spawn parallel
agents in the shared tree") which is a process-side rule — it tells
agents what to do, but agents drift anyway because their harness's
shell state resets across bash calls and `cd` does not survive. The
process-side rule is necessary but not sufficient.

We need a host-side gate that refuses commits authored from the main
checkout while an isolated agent worktree is active, with a clean
bypass for the user's own legitimate main-checkout commits.

## Decision

Add a pre-commit hook
`scripts/ci/check-agent-worktree-drift.sh`, wired through
`.pre-commit-config.yaml` as a `local` repo hook (so
`make hooks-install` picks it up automatically). The hook resolves
the commit's toplevel via `git rev-parse --show-toplevel`. If the
toplevel matches `*/.claude/worktrees/agent-*`, the commit is allowed.
Otherwise (toplevel is the main checkout) the hook scans for sibling
worktrees under `<toplevel>/.claude/worktrees/agent-*`. If at least
one exists, the commit is refused with a clear error pointing to the
documented bypass and the per-agent recovery path. If none exist —
no agent kit on this clone, or no active agents — the commit passes.
Bypass for the human user is the standard `git commit --no-verify`.

Documentation lands at
[`docs/development/agent-worktree-discipline.md`](../development/agent-worktree-discipline.md);
agent-side guidance is added to `AGENTS.md` so any host (Cursor,
Aider, Continue, Codex, Claude Code) sees the same canonical pattern.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Process-side rule only (status quo) | Zero new code; lives in agent prompts and global memory. | Already failed five times in one session — agents drift faster than the rule can be reinforced. | Insufficient on its own; we keep it as Layer 1 alongside the new guard. |
| Refuse *all* commits from main while *any* agent worktree exists ever, no bypass | Strongest invariant. | Blocks the human user's legitimate commits to master from the main checkout — false positive on every release-please push, every doc-only edit by the user. | Too aggressive; conflicts with day-to-day human workflow. |
| Wrapper around `git` itself (shell alias, function) | Catches drift before staging, not just before commit. | Per-shell, per-user; doesn't survive a fresh agent process group; doesn't compose with `git -C` invocations from tool calls. | Brittle; easily bypassed by agents that exec `/usr/bin/git` directly. |
| `core.hooksPath` pointing at a tracked dir | One commit installs hooks for everyone, no `make hooks-install` step. | Conflicts with the existing `pre-commit` framework which owns `.git/hooks/pre-commit`; competes with the upstream pre-commit infra we already use for clang-format / lint. | Would require migrating off `pre-commit` framework — out of scope. |
| Server-side gate (GitHub action that rejects commits whose author-time `cwd` was main while branch had agent siblings) | Defence in depth. | No reliable signal for "the author's cwd was main" in a pushed commit; requires structured commit metadata that doesn't exist. | Infeasible. |

The chosen design — a `local` pre-commit hook with a narrow refusal
condition and the standard `--no-verify` bypass — is the smallest
surface that catches every observed drift incident without breaking
the human user's workflow.

## Consequences

- **Positive**: Drift commits are stopped at `git commit` time with a
  clear, actionable error. The user's main-checkout work is unaffected
  when no agent is running, and remains bypassable when an agent is
  running. Layer 1 (agent-side discipline) and Layer 2 (host-side
  guard) compose: the guard is a safety net, not a replacement for
  the rule.
- **Negative**: Adds one more hook to the pre-commit pipeline (~10 ms
  on a no-op commit). Human users committing legitimate main-checkout
  work while an agent is active must remember `--no-verify`.
- **Neutral / follow-ups**:
  - `make hooks-install` documentation in
    [`docs/development/agent-worktree-discipline.md`](../development/agent-worktree-discipline.md).
  - `AGENTS.md` gains a new "Worktree discipline" section pointing at
    the canonical agent-side pattern.
  - `scripts/ci/test_check_agent_worktree_drift.sh` self-tests the
    three branches; runs with the rest of the shellcheck / sanity
    targets in CI.
  - Stale agent worktrees on a developer machine (agents that
    finished but didn't `git worktree remove` themselves) will
    spuriously trip the guard against the human user's main-checkout
    commits. The error message documents the `git worktree remove`
    cleanup; a future ADR may add an automatic prune step.

## References

- Source: `req` — user task brief 2026-05-09 ("ship a hard guard
  against the agent-worktree-drift incident pattern — agents
  `cd`'ing into the main repo's working tree and committing there
  instead of their isolated worktree. 5 incidents this session
  (#498 AdaptiveCpp / #520 T3-15 / #526 ccache / MCP runtime v2 first
  attempt / multi-corpus). Each lost work or required cherry-pick
  recovery.").
- Global memory `feedback_agents_isolated_worktree_only` —
  process-side complement (Layer 1).
- Global memory `feedback_no_test_weakening` — informs the design
  rationale that the guard is additive to the process-side rule, not
  a substitute for it.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — fork-local PR
  deep-dive deliverables.
- [ADR-0221](0221-changelog-adr-fragment-pattern.md) — fragment-based
  CHANGELOG / ADR-index pattern.
- Related PRs: #498, #520, #526 (drift incidents).
