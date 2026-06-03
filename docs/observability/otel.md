# OpenTelemetry integration (ADR-0782)

VMAFX exports distributed traces and metrics via the OpenTelemetry SDK
(v1.44) over OTLP/gRPC.  All four Go binaries participate:
`vmafx-controller`, `vmafx-node`, `vmafx-server`, and `vmafx-mcp`.

## Quick start

### Self-hosted (Jaeger + Prometheus)

```bash
# Start an all-in-one OTLP collector
docker run -p 4317:4317 -p 16686:16686 \
  jaegertracing/all-in-one:latest

# Run vmafx-controller with tracing enabled
OTEL_EXPORTER_OTLP_ENDPOINT=localhost:4317 \
  ./vmafx-controller --port 8080
```

Open `http://localhost:16686` to view traces.

### Kubernetes (Helm sidecar)

```bash
helm upgrade --install vmafx ./deploy/helm/vmafx \
  --set otelCollector.enabled=true \
  --set otelCollector.config="$(cat my-collector-config.yaml)"
```

The sidecar listens on `localhost:4317` inside the pod; the VMAFX binary
connects there automatically (the default endpoint matches).

## Environment variables

| Variable | Default | Description |
| --- | --- | --- |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | `localhost:4317` | OTLP/gRPC collector endpoint. Set to `""` to disable (no-op providers are used). |
| `OTEL_SERVICE_NAME` | *binary name* | Overrides the `service.name` resource attribute. |

## Span names

| Span | Binary | Description |
| --- | --- | --- |
| `vmafx.job.submit` | controller | SubmitJob gRPC handler — covers queue persistence. |
| `vmafx.encoder.dispatch` | node | Encoder selection and ffmpeg invocation. |
| `vmafx.frame.extraction` | node | Inner span inside `vmafx.scoring` — libvmaf per-frame feature extraction. |
| `vmafx.scoring` | node | Full end-to-end scoring pipeline for one job. |
| `vmafx.onnx.inference` | node | ONNX Runtime inference (Stage 2). |

All spans carry the bounded attributes `vmafx.job_id`, `vmafx.model`,
`vmafx.backend`, `vmafx.node_id`.

## Metrics

OTel-native instruments (`pkg/observability.OTelMetrics`):

| Instrument | Type | Unit | Description |
| --- | --- | --- | --- |
| `vmafx.jobs.queued` | UpDownCounter | `{job}` | Pending jobs in the controller queue. |
| `vmafx.jobs.in_flight` | UpDownCounter | `{job}` | Jobs currently assigned to nodes. |
| `vmafx.score_latency_ms` | Histogram | `ms` | End-to-end scoring latency. Explicit buckets at 10/50/100/250/500/1000/2500/5000/10000 ms for p50/p99 discrimination. |
| `vmafx.frames_per_second` | Histogram | `fps` | Frame-extraction throughput. |
| `vmafx.gpu_utilization` | Gauge | `%` | Per-node GPU compute utilisation (0–100). |

Prometheus instruments (existing `/metrics` endpoint) are also extended with
`vmafx_controller_jobs_submitted_total`, `vmafx_controller_jobs_completed_total`,
`vmafx_controller_jobs_failed_total`, `vmafx_controller_jobs_queued`, and
`vmafx_controller_nodes_active`.

## Grafana dashboard

Import `deploy/grafana/vmafx-overview.json` into Grafana.  The dashboard
requires a Prometheus data source configured to scrape `/metrics` on the
controller (or via the OTel collector's Prometheus exporter).

## Cardinality budget

All span attributes and metric labels are bounded-cardinality:

- `vmafx.job_id` — present on spans only (not metrics).
- `vmafx.model` — at most ~10 VMAF model variants.
- `vmafx.backend` — at most 6 values (`cpu`, `cuda`, `sycl`, `hip`, `vulkan`, `metal`).
- `vmafx.gpu_vendor` — at most 4 values (`nvidia`, `amd`, `intel`, `cpu`).

No per-file or per-clip attributes are added to metrics.
