## Observability: one metric definition and the generated Overview dashboard (2026-10-07)

`rc4/obs-1-metric-definitions`, [ADR-2349](adr/2349-observability-package.md), #2430.

- Every Prometheus family is defined in `pkg/observability/metricdef`; the
  services register it through `pkg/observability`'s `NewCounter`,
  `NewGauge`, `NewHistogram` and `RegisterScraped`. A sync that brings back a
  `prometheus.New*Vec`, a GaugeFunc or `promauto` in a service bypasses the
  label bounds and fails the per-binary contract tests.
- `pkg/observability.NewMetrics` returns `(*Metrics, error)` and
  `SetControllerSources` is gone (the controller's queue families are in
  `cmd/vmafx-controller/metrics.go`). The controller queue's `Cancel` and
  `ReportResult` return `(bool, error)`; the bool feeds the job counters.
- `deploy/grafana/vmafx-overview.json` moved to
  `deploy/grafana/dashboards/vmafx-overview.json` and is generated, as is
  `docs/observability/metrics.md`: on a conflict take either side and run
  `go run ./tools/obsgen -write`, never merge by hand.
- `vmafx-node` composes `bootstrap.HTTP` (`nodeServerOptions`), listens on
  `VMAFX_HTTP_ADDR` (default `:9090`) and its Dockerfile stages expose 9090.
- No score, FFmpeg patch or C API impact.

<!-- markdownlint-disable-file MD013 MD041 -->
