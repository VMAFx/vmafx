- **The `Pre-Commit` CI job checks a pull request on its own diff
  (HISS-18).** `scripts/ci/precommit_scope.py` runs
  `pre-commit run --from-ref <base sha> --to-ref HEAD` on pull requests and
  keeps `--all-files` for master pushes, release tags, schedule and dispatch,
  so a documentation-only pull request no longer runs every hook over the whole
  tree. The required context still reports. See
  [CI](docs/development/ci.md#pre-commit-scope-hiss-18).
