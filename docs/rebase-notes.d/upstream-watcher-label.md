## Upstream watcher label creation and failure guard (2026-10-25)

- `scripts/upstream-watcher/ensure_label.sh` ensures `upstream-blocked` exists
  before `upstream-watcher.yml` opens tracking issues, avoiding failures when the
  label is missing.
- `scripts/ci/scheduled-failure-issue.sh` generalizes scheduled failure issue
  reporting (comment on open issue with the same title or create); `tidy-drift-issue.sh`
  is now a thin wrapper over it.
- `upstream-watcher.yml` adds the `scheduled-failure` guard job to notify maintainers
  when scheduled runs fail.
- Netflix upstream commits `5a50df8af` (`not-applicable`) and `f712fa756` (`ported`)
  are disposed; `f712fa756` ports the `Ubuntu gcc zimg` matrix build to
  `libvmaf-build-matrix.yml` and `required-aggregator.yml`.
