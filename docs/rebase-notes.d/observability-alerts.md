## Observability: generated alert rules and runbooks (2026-10-07)

`rc4/obs-3-alerts`, [ADR-2349](adr/2349-observability-package.md), #2430.

- `deploy/prometheus/vmafx-rules.yaml` and its promtool test
  `vmafx-rules.test.yaml` are generated (`pkg/observability/obsgen`): on a
  conflict take either side and run `go run ./tools/obsgen -write`.
- Every alert needs a page `docs/observability/runbooks/<slug>.md`
  (`TestEveryAlertHasARunbook`); renaming an alert renames its page and the
  mkdocs nav entry together.
- `scripts/ci/pinned-tool.sh` fetches both pinned tools (dashboard-linter,
  promtool); `build-config.env` pins `PROMETHEUS_VERSION` and its sha256.
- No score, FFmpeg patch or C API impact.
