---
paths:
  - .codex/hooks.json
  - .codex/hooks/*
  - scripts/ci/tests/test_codex_hook_config.py
invariant: Every Codex hook command resolves through active worktree root after clearing `GIT_DIR` / `GIT_WORK_TREE`.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# Codex repository-hook path contract

`.codex/hooks.json` must invoke every tracked hook through active Git
worktree root after clearing inherited repository-local Git variables.
command contract starts with `"$(env -u GIT_DIR -u GIT_WORK_TREE` and ends with
`git rev-parse --show-toplevel)/.codex/hooks/<script>.sh"`.
Never commit user-home path, former checkout path, or path relative to
launch directory. Keep exact seven event/matcher/script mappings, tracked
`100755` modes, and `scripts/ci/tests/test_codex_hook_config.py` together.
`test-codex-hook-config` pre-commit/pre-push hook is required local and CI
caller; JSON parse alone does not prove that commands execute from
nested directory or linked worktree.
