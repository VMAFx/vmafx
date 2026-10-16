- **pre-commit 4.7.0 in every lock; the hook-environment test models the old
  npm leak.** pre-commit 4.7.0 installs `language: node` hooks without the
  commit's git variables, so `scripts/githooks/tests/test_install_hooks_env.py`
  now reproduces the leak with a `pre-commit` wrapper that runs npm with the
  caller's environment. The `env -u` line of
  `scripts/git-hooks/framework-hooks.sh` stays as the second barrier. The same
  update brings ruff 0.17.0, semgrep 1.180.0, tox 4.65.4 and onnxruntime 1.31.0.
