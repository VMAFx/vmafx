## Observability: logs over OTLP and latency exemplars (2026-10-07)

`rc4/obs-4-logs-traces`, [ADR-2349](adr/2349-observability-package.md), #2430.

- `internal/app/bootstrap.Base` carries `otel.ModuleWithSlogBridge`; keep it
  when golusoris changes its otel module (golusoris#617 moves the bridge onto
  the injected `*slog.Logger`).
- Every `/metrics` mount uses `observability.MetricsHandler` (OpenMetrics,
  exemplars); `Metrics.ScoreDuration` became `ObserveScoreDuration(ctx, s)`,
  and `StreamMetrics.Begin` takes the call's context.
- `deploy/grafana/provisioning/datasources/vmafx.yaml` and
  `deploy/grafana/dashboards/vmafx-logs.json` are generated: regenerate, never
  merge by hand.
- No score, FFmpeg patch or C API impact.
