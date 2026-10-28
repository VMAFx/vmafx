## Pre-Commit CI job scoped to the pull request diff (2026-10-27)

- Fork-only CI change: the `pre-commit` job of `lint-and-format.yml` calls
  `scripts/ci/precommit_scope.py` instead of `pre-commit run --all-files`.
  Nothing upstream (Netflix) has these files; no rebase impact beyond keeping
  the fork's side of that step.
