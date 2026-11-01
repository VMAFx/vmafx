- CI: the `Pre-Commit` job checks the whole tree on a pull request that
  changes `.pre-commit-config.yaml` or a script a local hook runs. On its own
  diff a new hook passed on its pull request and failed the next master push
  (`check-fork-python-subprocess`, #3237); `scripts/ci/precommit_scope.py`
  now reads the pull request's file list and fails closed when git cannot
  give one.
