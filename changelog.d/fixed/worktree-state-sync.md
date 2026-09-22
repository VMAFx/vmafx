- Fixed post-commit governance synchronization from linked worktrees. The hook
  now uses regular, locked private-state mirrors, records the committing
  worktree's Git identity, preserves local caches, and fails visibly on
  symlinked or malformed state instead of updating the wrong checkout.
