- **Agent worktree-drift hard guard
  ([ADR-0332](../docs/adr/0332-agent-worktree-drift-hard-guard.md)).**
  New pre-commit hook `scripts/ci/check-agent-worktree-drift.sh`
  (wired through `.pre-commit-config.yaml` and installed by
  `make hooks-install`) refuses commits whose toplevel is the main
  checkout while sibling agent worktrees exist under
  `<repo-root>/.claude/worktrees/agent-*`. Catches the drift pattern
  observed five times in the 2026-05-09 session where a background
  agent committed into the main checkout instead of its assigned
  worktree. Bypass via the standard `git commit --no-verify` for
  legitimate human main-checkout commits while an agent runs.
  Documented at
  [`docs/development/agent-worktree-discipline.md`](../docs/development/agent-worktree-discipline.md);
  agent-side pattern added to `AGENTS.md`. Self-tests in
  `scripts/ci/test_check_agent_worktree_drift.sh` cover the three
  branches (agent-WT allow, main-WT-clean allow, main-WT-with-agent
  refuse).
