<!-- markdownlint-disable MD013 MD060 -->
# Observability — VMAFX Go services

This page is the operator guide for the telemetry the VMAFX Go binaries
emit: what each binary traces, which environment variables switch the
OpenTelemetry (OTel) export on, and how to point every binary at a
collector. It covers all seven binaries under `cmd/`:
`vmafx-server`, `vmafx-controller`, `vmafx-node`, `vmafx-operator`,
`vmafx-mcp`, `vmafx-tune`, and `vmafx-ort-runner`.

| Signal      | Mechanism                                             | Status                                                                                                                 |
|-------------|-------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------|
| **Logs**    | `log/slog` via golusoris's log module (stderr/stdout) | Production, every binary. Not exported over OTLP (no slog bridge is wired; see [Logs](#logs)).                         |
| **Metrics** | Prometheus `/metrics` endpoint                        | Production on `vmafx-server`, `vmafx-controller` and `vmafx-node`, built from `pkg/observability/metricdef` (see [Metrics](#metrics)). |
| **Traces**  | OpenTelemetry, OTLP/gRPC → your collector             | Production on every binary except `vmafx-ort-runner` (exempt, see [below](#vmafx-ort-runner-is-exempt)). Off by default. |

Design decisions: [ADR-0782](../adr/0782-otel-tracing.md) (span and
attribute schema, best-effort/non-blocking rule),
[ADR-0927](../adr/0927-opentelemetry-traces-metrics-phase1.md) (the
per-service rollout plan), [ADR-1095](../adr/1095-otel-grpc-trace-context.md)
(cross-process propagation), and
[ADR-1119](../adr/1119-golusoris-go-framework-adoption.md) (the golusoris
fx framework whose `otel.Module` now does the wiring).

## How OTel is wired (one place)

Every fx binary composes `internal/app/bootstrap.Base`, which carries the
[golusoris](https://github.com/golusoris/golusoris) `otel.Module`
(ADR-1119). That module builds the OTLP/gRPC trace, metric and log
exporters, installs the SDK providers as the OTel globals, installs the
W3C `TraceContext` + `Baggage` propagators, and registers an fx `OnStop`
hook that flushes and shuts the providers down when the process exits.
`bootstrap.Base` additionally completes the resource identity:
`service.name` is derived from the binary name (or the overrides below)
and `service.version` comes from `pkg/version`, i.e. the same string
`--version` prints.

`vmafx-tune` is a cobra CLI; each subcommand builds the same
`bootstrap.Base` graph for its own lifetime, so a tune invocation
initialises OTel on start and flushes it on exit like the long-running
services do. No binary carries a private OTel init — `cmd/AGENTS.md`
records that as an invariant.

### No collector, no cost

When no OTLP endpoint is configured the module installs **no-op
providers**: no exporter is created, nothing is dialled, spans are never
sampled, and the only trace of OTel is one `otel: configured … active=false`
log line at startup. Set `OTEL_SDK_DISABLED=true` to force the same
behaviour even when an endpoint is present. This is the ADR-0782
"best-effort and non-blocking" rule: a missing or unreachable collector
never prevents a binary from starting or serving.

## Pointing the binaries at a collector

1. **Run an OTel collector** that accepts OTLP over gRPC. For local
   development the `otel/opentelemetry-collector-contrib` image with the
   `debug` exporter prints every span it receives:

    ```bash
    docker run --rm -p 4317:4317 \
      otel/opentelemetry-collector-contrib:latest
    ```

   Jaeger 1.50+, Grafana Tempo, SigNoz, and the hosted vendors
   (Honeycomb, Grafana Cloud, Datadog, New Relic, …) all ingest OTLP
   directly, so the collector can also be the backend itself.

2. **Set the endpoint.** Either the OTel-standard variable or the vmafx
   config key works; the standard one is what the Helm chart passes
   through:

    ```bash
    export OTEL_EXPORTER_OTLP_ENDPOINT=localhost:4317
    ./vmafx-server
    ```

   The exporter speaks **OTLP/gRPC** (default collector port **4317**,
   not the OTLP/HTTP port 4318) and dials **plaintext** by default because
   collectors normally live in-cluster; set `VMAFX_OTEL_INSECURE=false`
   for TLS.

3. **Verify.** Every binary logs one line at startup:

    ```text
    INFO otel: configured enabled=true active=true endpoint=localhost:4317 service=vmafx-server
    ```

   `active=false` means the endpoint was not seen (typo, wrong variable
   name, or `OTEL_SDK_DISABLED`). Then make a request — a gRPC `Health`
   call, `GET /v1/health`, an MCP tool call, any `vmafx-tune` subcommand —
   and the collector prints a span batch within the 5 s batch timeout.

### Kubernetes / Helm

The chart (`deploy/helm/vmafx`) exposes a generic `env` map on every
workload, so the endpoint is one value:

```bash
helm upgrade --install vmafx ./deploy/helm/vmafx \
  --set env.OTEL_EXPORTER_OTLP_ENDPOINT=otel-collector.observability:4317
```

`otelCollector.enabled=true` renders the collector **ConfigMap**
(`templates/otel-collector-sidecar.yaml`) with a default OTLP/gRPC →
`debug` pipeline; it does not inject a collector container into the pods,
so deploy the collector (sidecar or DaemonSet) yourself and point `env`
at it as above. The golusoris resource detector also reads the
downward-API variables `POD_NAME`, `POD_NAMESPACE`, `POD_IP`, `NODE_NAME`
and `SERVICE_ACCOUNT` (the chart does not set them; add them through your
own manifests) and turns them into
`k8s.pod.name`, `k8s.namespace.name`, `k8s.pod.ip`, `k8s.node.name` and
`k8s.service_account.name` resource attributes.

## Configuration reference

All binaries read the same keys. The vmafx keys are golusoris config
keys under the `VMAFX_` prefix (`VMAFX_OTEL_SAMPLE_RATIO` → `otel.sample.ratio`);
the standard `OTEL_*` variables are read by the OTel SDK / exporter
itself, or by `bootstrap.Base` where noted.

| Variable                                             | Default                          | Meaning                                                                                                                                        |
|------------------------------------------------------|----------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------|
| `OTEL_EXPORTER_OTLP_ENDPOINT`                        | _(unset → no-op)_                | OTLP/gRPC collector target (`host:4317`). Per-signal variants `OTEL_EXPORTER_OTLP_{TRACES,METRICS,LOGS}_ENDPOINT` also count as "configured". |
| `VMAFX_OTEL_ENDPOINT`                                | _(unset)_                        | Same target through the vmafx config key. Either variable switches export on.                                                                  |
| `VMAFX_OTEL_INSECURE`                                | `true`                           | Plaintext gRPC to the collector. `false` dials with TLS.                                                                                       |
| `VMAFX_OTEL_ENABLED`                                 | `true`                           | Master switch. `false` is a no-op even with an endpoint.                                                                                      |
| `OTEL_SDK_DISABLED`                                  | `false`                          | OTel-standard kill switch; `true` forces the no-op providers.                                                                                  |
| `OTEL_SERVICE_NAME`                                  | _(binary name)_                  | `service.name` resource attribute (honoured by `bootstrap.Base`).                                                                              |
| `VMAFX_OTEL_SERVICE_NAME`                            | _(binary name)_                  | Same attribute through the vmafx key; wins over `OTEL_SERVICE_NAME`.                                                                           |
| `VMAFX_OTEL_SERVICE_VERSION`                         | `pkg/version` (`--version` text) | `service.version` resource attribute.                                                                                                          |
| `VMAFX_OTEL_SERVICE_NAMESPACE`                       | _(unset)_                        | `service.namespace` resource attribute.                                                                                                        |
| `VMAFX_OTEL_SAMPLE_RATIO`                            | `1.0`                            | Head-based, parent-respecting trace sample ratio in `[0.0, 1.0]`. **`OTEL_TRACES_SAMPLER_ARG` is not read** — golusoris installs its own sampler. |
| `VMAFX_OTEL_EXPORT_TRACES` / `_METRICS` / `_LOGS`    | `true`                           | Per-signal export toggles, e.g. `VMAFX_OTEL_EXPORT_LOGS=false` when the collector rejects the logs signal.                                    |
| `OTEL_RESOURCE_ATTRIBUTES`                           | _(unset)_                        | Extra `key=value` resource attributes (OTel standard).                                                                                          |
| `POD_NAME`, `POD_NAMESPACE`, `POD_IP`, `NODE_NAME`, `SERVICE_ACCOUNT` | _(unset)_       | Mapped to `k8s.*` resource attributes when present (Kubernetes downward API).                                                                  |

Fixed by the module (not configurable): 5 s trace batch timeout, 15 s
metric export interval, `ParentBased(TraceIDRatioBased(ratio))` sampler.

### Sampling

The default ratio is **1.0** — every root span is exported. That is the
right default for a scoring service whose request rate is bounded by
encode/score throughput, but for a busy controller start at
`VMAFX_OTEL_SAMPLE_RATIO=0.1` and tune from observed tail-latency
coverage. Child spans always inherit the parent's decision, so a trace is
exported whole or not at all, and a sampled inbound `traceparent` is
honoured.

## What each binary traces

Span names are defined once, in `pkg/observability/otel_instruments.go`
(the ADR-0782 schema), plus the names the upstream `otelgrpc` /
`otelhttp` instrumentation generates. Attributes are bounded-cardinality:
`vmafx.job_id` (spans only), `vmafx.model`, `vmafx.backend`,
`vmafx.node_id`, `vmafx.mcp.tool`, `vmafx.tune.command`.

| Binary               | OTel init                                             | Spans                                                                                                                                                                                                                                                                                              |
|----------------------|-------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `vmafx-server`       | `bootstrap.Base`                                      | gRPC server spans `vmafx.v1.VmafxScoring/{Score,ScoreStream,Health}` (golusoris `grpc.Module` → `otelgrpc`); HTTP server spans `GET /v1/health`, `GET /v1/ready`, `POST /v1/score`, `GET /swagger`, `GET /swagger/*` (`bootstrap.HTTPTracing`).                                                    |
| `vmafx-controller`   | `bootstrap.Base`                                      | gRPC server spans for `vmafx.v1.VmafxScoring/*` and `vmafx.controller.v1.VmafxController/{SubmitJob,GetJob,CancelJob,StreamJobs,RegisterNode,Heartbeat,PullWork,ReportResult}`; child span `vmafx.job.submit` (`vmafx.model`, `vmafx.backend`) inside `SubmitJob`; HTTP server spans `POST /v1/score`. |
| `vmafx-node`         | `bootstrap.Base`                                      | gRPC server spans `vmafx.v1.VmafxScoring/*`; `vmafx.scoring` → `vmafx.frame.extraction` around the libvmaf run, `vmafx.onnx.inference` around in-process ONNX inference (`vmafx.model`, `vmafx.backend`, `vmafx.job_id`).                                                                          |
| `vmafx-operator`     | `bootstrap.Base`                                      | gRPC client spans `vmafx.controller.v1.VmafxController/GetJob` for every `VmafxJob` poll (golusoris `ConnFactory` → `otelgrpc`), linked to the controller's server span. controller-runtime reconcile loops carry no span of their own.                                                             |
| `vmafx-mcp`          | `bootstrap.Base`                                      | `vmafx.mcp.tool` per tool call on both transports (`vmafx.mcp.tool` = tool name; failures on the span status while the MCP `isError` response is unchanged); on the HTTP transport an outer `POST /` server span from `bootstrap.TraceHTTPHandler`.                                                  |
| `vmafx-tune`         | `bootstrap.Base` per subcommand (`withGolusoris`)     | `vmafx.tune.command` around the whole run (`vmafx.tune.command` = cobra path, e.g. `vmafx-tune-go sidecar status`); `vmafx.onnx.inference` (`vmafx.model`) from `pkg/ai` around each `vmafx-ort-runner` subprocess.                                                                                 |
| `vmafx-ort-runner`   | **exempt** ([ADR-1134](../adr/1134-vmafx-ort-runner-in-tree.md)) | none — see below.                                                                                                                                                                                                                                                                                  |

Kubernetes probes and the Prometheus scrape (`/healthz`, `/readyz`,
`/livez`, `/startupz`, `/metrics`) are never traced.

### Cross-process propagation

Every gRPC server (golusoris `grpc.Module`) installs the `otelgrpc`
server stats handler and every gRPC client the fork dials
(`pkg/score`, golusoris `ConnFactory` in the operator) installs the client
handler, so the W3C `traceparent` crosses each hop and a scoring job shows
up as one trace: operator poll → controller `GetJob`, client `SubmitJob` →
controller `vmafx.job.submit`, controller/server `Score` → node
`vmafx.scoring`. HTTP callers can inject `traceparent` themselves; the
`otelhttp` server span parents to it.

### `vmafx-ort-runner` is exempt

`vmafx-ort-runner` is a millisecond-lived subprocess spawned once per
ONNX inference, deliberately kept framework-free (ADR-1134: stdlib `flag`
only, no config, no logger, nothing to inject). Initialising an exporter
in it would add a config load and an export flush to every predictor
call for a span whose parent lives in the caller anyway, and there is no
argv-level trace propagation to link it. Instead the **caller** owns the
span: `pkg/ai.Registry.Infer` wraps the subprocess in
`vmafx.onnx.inference`, and the node's in-process inference path emits the
same name, so ONNX latency is visible in every trace without the runner
participating.

## Metrics

Three binaries serve a Prometheus `/metrics` page on their HTTP listener:

| Binary | Listener (`VMAFX_HTTP_ADDR`) | Families |
|--------|------------------------------|----------|
| `vmafx-server` | `:8080` | Score requests, errors and latency; the quality family; `vmafx_build_info`. |
| `vmafx-controller` | `:8080` | The server's families, plus the job queue: submitted, completed, failed, cancelled and requeued jobs, pending and running jobs and the age of the oldest pending job per tenant, queue wait and time to result, live nodes. |
| `vmafx-node` | `:9090` | Backend and vendor, slots, running jobs, jobs by backend and outcome, job run time. |

Each also serves the Go runtime and process series of the Prometheus client
library. The [metric reference](../observability/metrics.md) lists every
family with its type, unit, labels and cardinality bound.

```bash
curl -s localhost:8080/metrics | grep '^vmafx_controller_jobs_pending'
# vmafx_controller_jobs_pending{tenant="acme"} 3
```

All names come from one definition, `pkg/observability/metricdef`: the
services build their collectors from it, the dashboards are generated from it
and the reference page is rendered from it. A family is added there first,
then registered by the binaries its `Emitters` name; a test per binary fails
when its `/metrics` page and the definition disagree.

### Label cardinality

Every label is bounded. A label with a closed value set (`backend`,
`outcome`, `reason`, `vendor`) maps any other value to `other`. An open label
reports at most a fixed number of distinct values per process, `tenant` 32
and `model` 16, and merges every later value into `other`: counts add up,
the age of the oldest job takes the oldest, nothing is dropped. The largest
family, `vmafx_quality_score`, stays below 11 000 series per process. An
empty value (a request to `vmafx-server`, which has no tenants) reads `none`.

### Per-tenant series

The controller's job counters and queue gauges carry a `tenant` label. A
query written against the totals keeps working once wrapped in `sum()`:
`sum(vmafx_controller_jobs_pending)`.

## Dashboards

The dashboards under `deploy/grafana/dashboards/` are generated in Go with
the [Grafana Foundation SDK](https://github.com/grafana/grafana-foundation-sdk)
(v0.0.20, Apache-2.0) from the metric definition, never edited by hand:

```bash
go run ./tools/obsgen -write   # regenerate dashboards and the metric reference
go run ./tools/obsgen -check   # exit 1 when a committed file is stale
```

To use one, import its JSON in Grafana (_Dashboards → New → Import_) and pick
your Prometheus data source in the **Prometheus data source** variable. The
**Job** and **Instance** variables list the scrape jobs and targets that
serve `vmafx_build_info`; **Tenant** lists the tenants that submitted jobs.
Each restart of a component is marked by the _Deploys and restarts_
annotation with the version it started, so a change in a graph can be read
against a rollout.

A test (`TestEveryDashboardQueryIsEmitted` in `pkg/observability/obsgen`)
fails when any panel, annotation or variable of a shipped dashboard queries a
series that no binary emits; the dashboard as it was before generation is
kept as its negative fixture.

### Overview

`vmafx-overview.json` answers, row by row:

| Row | Panels | Question |
|-----|--------|----------|
| Health and queue | Components up, Live nodes, Pending jobs, Running jobs, Oldest pending job, Score request errors | Is every component scraped, can queued work run, is the queue draining? |
| Throughput | Jobs per minute, Job failure ratio by tenant, Jobs returned to the queue, Score requests per second | How much work finishes, and how much fails or is retried? |
| Latency | Queue wait, Time to result, Score request latency, Node job run time | How long does work wait and take? |
| Scores and nodes | Median score by model, Node slot use | Did the scores move, and are the nodes the bottleneck? |

A growing _Oldest pending job_ with _Live nodes_ above zero and low _Node
slot use_ means no node can take the queued work: compare the jobs' backend
with the nodes' (`vmafx_node_info`).

## Logs

Logs stay on the golusoris slog stream (stderr for `vmafx-mcp` on stdio,
stdout otherwise). The OTLP log exporter is initialised alongside traces
and metrics but no slog → OTel bridge is wired (ADR-0927 Phase 3), so no
log records are exported; set `VMAFX_OTEL_EXPORT_LOGS=false` to skip the
exporter entirely if the collector has no logs pipeline.

`vmafx-server` and `vmafx-controller` also log `write JSON response` or
`write probe response` when a client disconnect or socket failure prevents an
HTTP response from being written. The response status/body contract is
unchanged; these messages make a previously silent transport failure visible.

## Disabling OTel entirely

- Leave every endpoint variable unset (default) — no-op providers, one
  `active=false` log line.
- Set `OTEL_SDK_DISABLED=true` or `VMAFX_OTEL_ENABLED=false` when an
  unrelated process in the same pod injects the endpoint.

## Verifying the wiring without a collector

Each binary's test package proves its composition root inherits the
no-op providers and the expected `service.name` / `service.version`
(`TestOTelWiredThroughBootstrap`), and the request paths are exercised
against an in-memory span recorder (`internal/oteltest`):

```bash
go test ./internal/app/bootstrap/ ./cmd/vmafx-tune/cmd/ ./pkg/ai/ ./cmd/vmafx-operator/ \
  -run 'OTel|Span|ServiceIdentity|HTTPTracing'
# cgo packages need libvmaf on the link path, as in go-ci.yml:
CGO_LDFLAGS="-L$PWD/core/build-cpu/src -lvmaf -lm" \
  LD_LIBRARY_PATH=$PWD/core/build-cpu/src \
  go test ./cmd/vmafx-server/ ./cmd/vmafx-controller/ ./cmd/vmafx-node/ ./cmd/vmafx-mcp/ \
  -run 'OTelWired|EmitsServerSpan|EmitsLinkedSpans|ToolCallEmitsSpan'
```

## See also

- [ADR-0782](../adr/0782-otel-tracing.md) — span / attribute / metric schema.
- [ADR-0927](../adr/0927-opentelemetry-traces-metrics-phase1.md) — rollout plan and sampling rationale.
- [ADR-1095](../adr/1095-otel-grpc-trace-context.md) — gRPC trace-context propagation.
- [ADR-1119](../adr/1119-golusoris-go-framework-adoption.md) — golusoris fx adoption (`otel.Module`, `bootstrap.Base`).
- [ADR-1134](../adr/1134-vmafx-ort-runner-in-tree.md) — why `vmafx-ort-runner` stays framework-free.
- [OpenTelemetry schema page](../observability/otel.md) — span names and attributes, OTel instruments.
- [Metric reference](../observability/metrics.md) — every Prometheus family, generated from `pkg/observability/metricdef`.
- [`internal/app/bootstrap/bootstrap.go`](../../internal/app/bootstrap/bootstrap.go) — the shared composition stanza.
- [`pkg/observability/otel_instruments.go`](../../pkg/observability/otel_instruments.go) — span names and attribute keys.
