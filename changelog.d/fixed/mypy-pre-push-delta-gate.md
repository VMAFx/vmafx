- **The local pre-push type check no longer blocks a branch over findings it did
  not introduce, and no longer refuses `ai/src/` outright.** `mypy` in CI is
  advisory by design, because numpy, pandas and torch stub coverage is uneven, so
  the number of findings a checkout reports depends on which of those packages it
  has installed. The blocking local hook reported all of them: on a checkout with
  those packages present, the 23 files one branch touched produced 14 findings,
  and `master`'s own copies of the same files produced the same 14. Separately, no
  branch touching a file under `ai/src/` could be pushed at all, because that
  directory is a `mypy_path` base and mypy refuses a path it can name two ways.
  The hook now runs those files with `--explicit-package-bases`, re-checks the
  same files at the merge base, and fails only on findings that are new; a
  non-zero exit it cannot attribute to a file still fails closed. See
  [ADR-1261](docs/adr/1261-mypy-pre-push-delta-gate.md).
