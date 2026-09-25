- Fixed all seven repository-local Codex hooks silently targeting the retired
  `/home/kilian/dev/vmaf` checkout. Hook commands now resolve the active Git
  worktree, with a pre-commit contract guarding mappings and executable modes.
