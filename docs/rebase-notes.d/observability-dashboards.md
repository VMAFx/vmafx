## Observability: dashboards, GPU exporters and scraped read errors (2026-10-07)

`rc4/obs-2-dashboards`, [ADR-2349](adr/2349-observability-package.md), #2430.

- `pkg/observability.RegisterScraped` takes a read-error counter
  (`NewReadErrors`) and a `ScrapeGroup`; a failed read counts under its source
  and never returns an invalid metric (that fails the whole `/metrics` page).
- `vmafx-server` and `vmafx-node` record ScoreStream sessions through
  `internal/app/scoringservice.StreamMetrics`; a sync that touches either
  `ScoreStream` handler keeps `Begin` / `defer End(retErr)` and the per-frame
  `Frame` call.
- The dashboards under `deploy/grafana/dashboards/` (now seven) are generated:
  on a conflict take either side and run `go run ./tools/obsgen -write`.
  `build-config.env` pins `DASHBOARD_LINTER_VERSION` and its sha256.
- No score, FFmpeg patch or C API impact.
