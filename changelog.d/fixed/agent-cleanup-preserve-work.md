- Agent-state cleanup now inventories by default and requires `--apply` with
  exact `--worktree` paths before removing eligible clean checkouts. It preserves
  modified, untracked and ignored files, active or unknown owners, detached work
  without preserving refs, and all stashes and branches. Failed removal restores
  the original owner lock (ADR-1239).
