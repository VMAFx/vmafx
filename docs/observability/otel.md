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

The configuration reference — every environment variable, how to point the
binaries at a collector, and the per-binary span table — is
[docs/development/observability.md](../development/observability.md); the
[observability operator guide](index.md) covers installing the monitoring.
This page keeps the schema.

## Quick start

```bash
# Any OTLP/gRPC receiver; Jaeger's all-in-one ingests OTLP natively.
docker run -p 4317:4317 -p 16686:16686 jaegertracing/all-in-one:latest

# Export is off until an endpoint is set; plaintext gRPC by default.
OTEL_EXPORTER_OTLP_ENDPOINT=http://localhost:4317 ./vmafx-controller
```

Open `http://localhost:16686` to view traces. In Kubernetes, pass the
same variable through the chart's `env` map
(`--set env.OTEL_EXPORTER_OTLP_ENDPOINT=http://otel-collector:4317`);
`--set otelCollector.enabled=true` renders a collector ConfigMap you can
mount into a collector sidecar or DaemonSet.

## Environment variables

| Variable | Default | Description |
| --- | --- | --- |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | _(unset → no-op providers)_ | OTLP/gRPC collector endpoint as a URL (`http://host:4317`). Without a scheme the SDK finds no host and sends to `localhost:4317`. Unset means no exporter, no network, no spans. |
| `OTEL_SERVICE_NAME` | _binary name_ | `service.name` resource attribute; `VMAFX_OTEL_SERVICE_NAME` wins. |
| `OTEL_SDK_DISABLED` | `false` | `true` forces no-op providers. |

The `VMAFX_OTEL_*` variables (endpoint, TLS, service name and version, sample
ratio, per-signal export) are rows of every Go binary's generated
[environment table](../usage/env-vars.md#go-services-golusoris); the
[configuration reference](../development/observability.md#configuration-reference)
explains the library behaviour behind them.

## Span names

| Span | Binary | Description |
| --- | --- | --- |
| `vmafx.job.submit` | controller | `SubmitJob` gRPC handler — covers queue persistence. |
| `vmafx.encoder.dispatch` | _(none)_ | Name defined (`SpanEncoderDispatch`) but no binary starts it yet. |
| `vmafx.frame.extraction` | node | Inner span inside `vmafx.scoring` — libvmaf per-frame feature extraction. |
| `vmafx.scoring` | node | Full end-to-end scoring pipeline for one job. |
| `vmafx.onnx.inference` | node, `pkg/ai` (tune) | ONNX Runtime inference: in-process on the node; around the `vmafx-ort-runner` subprocess in `pkg/ai`. |
| `vmafx.mcp.tool` | mcp | One MCP tool call (`vmafx.mcp.tool` = tool name), stdio and HTTP transports. |
| `vmafx.tune.command` | tune | One `vmafx-tune` subcommand invocation (`vmafx.tune.command` = cobra command path). |
| `<package>.<Service>/<Method>` | server, controller, node (server side); operator, `pkg/score` (client side) | gRPC spans from `otelgrpc`, e.g. `vmafx.v1.VmafxScoring/Score`, `vmafx.controller.v1.VmafxController/GetJob`. |
| `<METHOD> <path>` | server, controller, mcp (HTTP transport) | HTTP server spans from `otelhttp`, e.g. `POST /v1/score`; probes and `/metrics` are filtered out. |

All `vmafx.*` spans carry only the bounded attributes `vmafx.job_id`,
`vmafx.model`, `vmafx.backend`, `vmafx.node_id`, `vmafx.mcp.tool`,
`vmafx.tune.command`. `pkg/observability/otel_instruments.go` also defines
`vmafx.gpu_vendor` and `vmafx.status`, which no span sets today.

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
them yet (`InitOTelMetrics` has no production caller). The production
metrics path is the Prometheus `/metrics` page of `vmafx-server`,
`vmafx-controller` and `vmafx-node`; every family it serves, with its labels
and cardinality bound, is in the [metric reference](metrics.md), generated
from `pkg/observability/metricdef`.

## Grafana dashboards

The dashboards under `deploy/grafana/dashboards/` are generated from the same
metric definition; import them as described in the
[observability guide](../development/observability.md#dashboards).

## Cardinality budget

All span attributes are bounded-cardinality (the Prometheus label bounds are
in the [metric reference](metrics.md#labels)):

- `vmafx.job_id` — present on spans only (not metrics).
- `vmafx.model` — at most ~10 VMAF model variants.
- `vmafx.backend` — at most 5 values (`cpu`, `cuda`, `sycl`, `hip`, `metal`).
- `vmafx.gpu_vendor` — at most 4 values (`nvidia`, `amd`, `intel`, `cpu`).
- `vmafx.mcp.tool` — the registered MCP tool list.
- `vmafx.tune.command` — the `vmafx-tune` subcommand tree.

No per-file or per-clip attribute is added to a metric.
