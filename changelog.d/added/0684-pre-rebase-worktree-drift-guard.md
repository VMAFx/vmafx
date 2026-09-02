- **Pre-rebase worktree-drift guard** (ADR-0684) — companion git hook
  to ADR-0332 that refuses `git rebase` issued from the main checkout
  while one or more `.claude/worktrees/agent-*` worktrees are active.
  Catches the 2026-05-22 drift incident class where a background
  agent's `git rebase origin/master` mutated the user's main-checkout
  working tree across 200+ files before any `git commit` attempt
  could fire the existing pre-commit guard. Hook ships at
  `scripts/git-hooks/pre-rebase` and installs via `make hooks-install`
  alongside the existing `pre-push` hook. Bypass for legitimate
  human-user main-checkout rebases: `git rebase --no-verify`.
