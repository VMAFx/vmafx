- A changelog fragment filed under a directory that is not a Keep-a-Changelog section now
  **fails** the run instead of printing a warning and exiting 0. The old behaviour silently
  dropped the entry: `changelog.d/docs/retrain-runbook-1246.md` had been on `master` since
  PR #1313 and never rendered, and `--check` still passed because it compares rendered
  output against `CHANGELOG.md` and both agreed the entry did not exist. That fragment is
  restored to `changelog.d/added/`, so the retrain-runbook entry appears in the Unreleased
  block for the first time. See
  [ADR-1198](docs/adr/1198-changelog-unknown-section-is-an-error.md).
