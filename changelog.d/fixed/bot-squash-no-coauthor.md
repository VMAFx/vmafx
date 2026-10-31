- CI: squash commits merged automatically by Renovate or scheduled render jobs
  now carry an explicit commit body with sign-off and no tool or bot
  `Co-authored-by:` lines, and pushes to `master` verify landed squash bodies
  against disallowed trailers ([ADR-3389](docs/adr/3389-landed-commit-message-gate.md)).
