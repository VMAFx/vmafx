- **Observability: logs over OTLP, a Logs dashboard and latency exemplars
  (RC4, ADR-2349, #2430).** Every Go service bridges its slog records to the
  OpenTelemetry collector (golusoris's slog bridge in `bootstrap.Base`), so
  logs reach Loki with the trace and span of their context. The Score request
  and ScoreStream session latencies carry the trace of a sampled request as an
  exemplar, and every `/metrics` page serves OpenMetrics. A generated Logs
  dashboard (volume per level, errors, the lines of one trace) and a
  provisioning file linking the Prometheus, Tempo and Loki data sources
  (exemplar to trace, trace to logs, log to trace) ship under
  `deploy/grafana/`. See [logs](docs/development/observability.md#logs).
