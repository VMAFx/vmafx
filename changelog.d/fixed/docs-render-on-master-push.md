- CI: `docs-render-pr.yml` now runs on every push to `master` in addition to
  its hourly schedule fallback, so dropped GitHub scheduled runs no longer
  leave an open render pull request `BEHIND` past the 24-hour render bound
  (#3462, ADR-3552).
