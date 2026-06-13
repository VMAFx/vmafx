### Added — OpenTelemetry tracing and metrics (ADR-0782)

Wire `go.opentelemetry.io/otel` v1.44 across all four Go binaries
(vmafx-controller, vmafx-node, vmafx-server, vmafx-mcp):

- **Distributed traces**: five span types — `vmafx.job.submit`,
  `vmafx.encoder.dispatch`, `vmafx.frame.extraction`, `vmafx.scoring`,
  `vmafx.onnx.inference`.
- **OTel-native metrics**: `vmafx.jobs.queued`, `vmafx.jobs.in_flight`,
  `vmafx.score_latency_ms` (histogram, p50/p99-ready), `vmafx.frames_per_second`,
  `vmafx.gpu_utilization` (gauge).
- **Prometheus additions**: `vmafx_controller_jobs_submitted_total`,
  `vmafx_controller_jobs_completed_total`, `vmafx_controller_jobs_failed_total`,
  `vmafx_controller_jobs_queued` (gauge), `vmafx_controller_nodes_active` (gauge).
- **OTLP exporter**: `OTEL_EXPORTER_OTLP_ENDPOINT` env var (default
  `localhost:4317`); non-fatal — missing collector never blocks startup.
- **Helm sidecar**: optional OTel collector sidecar via
  `--set otelCollector.enabled=true`.
- **Grafana dashboard**: `deploy/grafana/vmafx-overview.json` — jobs, latency
  p50/p99, fps, and GPU utilisation panels.
