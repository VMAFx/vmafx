- **The `actionlint` hook and `make lint-actions` cannot hang
  ([ADR-2199](docs/adr/2199-actionlint-bounded-run.md)).** They run actionlint
  through `scripts/ci/run_actionlint.py`, which gives it 90 seconds
  (`ACTIONLINT_TIMEOUT_S`) and fails with exit 124 and the cause named instead
  of hanging a commit or a push when a `run:` script is larger than the pipe
  actionlint writes it to (a user over `fs.pipe-user-pages-soft`).
