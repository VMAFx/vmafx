- **`scripts/dev/cleanup-agent-state.sh`**: developer utility that removes
  stale agent worktrees (locked with a dead PID) and drops redundant git
  stashes (on `master`, detached HEAD, or branches that still exist locally).
  Supports `--dry-run` to preview changes without modifying repository state.
  See `docs/development/agent-worktree-discipline.md` for usage context.
