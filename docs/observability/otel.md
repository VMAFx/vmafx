<!-- markdownlint-disable MD013 -->
# OpenTelemetry integration (ADR-0782)

VMAFX exports distributed traces (and, when a collector is configured,
OTel metrics and an as-yet-unbridged logs pipeline) through the
OpenTelemetry Go SDK over **OTLP/gRPC**. Every Go binary participates
through the shared `internal/app/bootstrap.Base` composition
([ADR-1119](../adr/1119-golusoris-go-framework-adoption.md)):
`vmafx-server`, `vmafx-controller`, `vmafx-node`, `vmafx-operator`,
`vmafx-mcp` and `vmafx-tune`. `vmafx-ort-runner` is exempt
([ADR-1134](../adr/1134-vmafx-ort-runner-in-tree.md)); its caller emits
the inference span.

The operator guide — every environment variable, how to point the
binaries at a collector, and the per-binary span table — is
[docs/development/observability.md](../development/observability.md).
This page keeps the schema.

## Quick start

```bash
# Any OTLP/gRPC receiver; Jaeger's all-in-one ingests OTLP natively.
docker run -p 4317:4317 -p 16686:16686 jaegertracing/all-in-one:latest

# Export is off until an endpoint is set; plaintext gRPC by default.
OTEL_EXPORTER_OTLP_ENDPOINT=localhost:4317 ./vmafx-controller
```

Open `http://localhost:16686` to view traces. In Kubernetes, pass the
same variable through the chart's `env` map
(`--set env.OTEL_EXPORTER_OTLP_ENDPOINT=otel-collector:4317`);
`--set otelCollector.enabled=true` renders a collector ConfigMap you can
mount into a collector sidecar or DaemonSet.

## Environment variables

| Variable | Default | Description |
| --- | --- | --- |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | _(unset → no-op providers)_ | OTLP/gRPC collector endpoint (`host:4317`). Unset means no exporter, no network, no spans. |
| `VMAFX_OTEL_ENDPOINT` | _(unset)_ | Same, through the vmafx config key. |
| `OTEL_SERVICE_NAME` / `VMAFX_OTEL_SERVICE_NAME` | _binary name_ | `service.name` resource attribute (the vmafx key wins). |
| `VMAFX_OTEL_SERVICE_VERSION` | `pkg/version` | `service.version` resource attribute. |
| `VMAFX_OTEL_SAMPLE_RATIO` | `1.0` | Parent-based trace sample ratio. |
| `OTEL_SDK_DISABLED` | `false` | `true` forces no-op providers. |

The full table, including TLS, per-signal toggles and the Kubernetes
downward-API attributes, is in the operator guide.

## Span names

| Span | Binary | Description |
| --- | --- | --- |
| `vmafx.job.submit` | controller | `SubmitJob` gRPC handler — covers queue persistence. |
| `vmafx.encoder.dispatch` | node | Encoder selection and ffmpeg invocation. |
| `vmafx.frame.extraction` | node | Inner span inside `vmafx.scoring` — libvmaf per-frame feature extraction. |
| `vmafx.scoring` | node | Full end-to-end scoring pipeline for one job. |
| `vmafx.onnx.inference` | node, `pkg/ai` (tune) | ONNX Runtime inference: in-process on the node; around the `vmafx-ort-runner` subprocess in `pkg/ai`. |
| `vmafx.mcp.tool` | mcp | One MCP tool call (`vmafx.mcp.tool` = tool name), stdio and HTTP transports. |
| `vmafx.tune.command` | tune | One `vmafx-tune` subcommand invocation (`vmafx.tune.command` = cobra command path). |
| `<package>.<Service>/<Method>` | server, controller, node (server side); operator, `pkg/score` (client side) | gRPC spans from `otelgrpc`, e.g. `vmafx.v1.VmafxScoring/Score`, `vmafx.controller.v1.VmafxController/GetJob`. |
| `<METHOD> <path>` | server, controller, mcp (HTTP transport) | HTTP server spans from `otelhttp`, e.g. `POST /v1/score`; probes and `/metrics` are filtered out. |

All `vmafx.*` spans carry only the bounded attributes `vmafx.job_id`,
`vmafx.model`, `vmafx.backend`, `vmafx.node_id`, `vmafx.mcp.tool`,
`vmafx.tune.command`.

## Metrics

OTel-native instruments (`pkg/observability.OTelMetrics`):

| Instrument | Type | Unit | Description |
| --- | --- | --- | --- |
| `vmafx.jobs.queued` | UpDownCounter | `{job}` | Pending jobs in the controller queue. |
| `vmafx.jobs.in_flight` | UpDownCounter | `{job}` | Jobs currently assigned to nodes. |
| `vmafx.score_latency_ms` | Histogram | `ms` | End-to-end scoring latency. Explicit buckets at 10/50/100/250/500/1000/2500/5000/10000 ms for p50/p99 discrimination. |
| `vmafx.frames_per_second` | Histogram | `fps` | Frame-extraction throughput. |
| `vmafx.gpu_utilization` | Gauge | `%` | Per-node GPU compute utilisation (0–100). |

These instruments are defined and unit-tested but no binary registers
them yet (`InitOTelMetrics` has no production caller); the Prometheus
`/metrics` endpoint remains the production metrics path, extended with
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
- `vmafx.mcp.tool` — the registered tool list (~15 values).
- `vmafx.tune.command` — the subcommand tree (~20 values).

No per-file or per-clip attributes are added to metrics.
