<!-- markdownlint-disable MD013 MD060 -->
# Observability — VMAFX Go services

This page is the reference for the telemetry the VMAFX Go binaries emit:
what each binary traces, which environment variables switch the
OpenTelemetry (OTel) export on, how to point every binary at a collector, and
how the metrics, dashboards and rules are generated and checked. To install
and use the monitoring, start with the
[observability operator guide](../observability/index.md). This page
covers all seven binaries under `cmd/`:
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
    export OTEL_EXPORTER_OTLP_ENDPOINT=http://localhost:4317
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
  --set env.OTEL_EXPORTER_OTLP_ENDPOINT=http://otel-collector.observability:4317
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
keys under the `VMAFX_` prefix (`VMAFX_OTEL_SAMPLE_RATIO` → `otel.sample.ratio`)
and are rows of every Go binary's generated
[environment table](../usage/env-vars.md#go-services-golusoris) (enable switch,
endpoint, TLS, service name, version and namespace, sample ratio, per-signal
export). The standard `OTEL_*` variables below are read by the OTel SDK and
exporter themselves, or by `bootstrap.Base` where noted.

| Variable                                                              | Default                          | Meaning                                                                                                                                                                                                           |
|-----------------------------------------------------------------------|----------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `OTEL_EXPORTER_OTLP_ENDPOINT`                                         | _(unset → no-op)_                | OTLP/gRPC collector target as a URL (`http://host:4317`); without a scheme the SDK sends to `localhost:4317`. Per-signal variants `OTEL_EXPORTER_OTLP_{TRACES,METRICS,LOGS}_ENDPOINT` also count as "configured". |
| `OTEL_SDK_DISABLED`                                                   | `false`                          | OTel-standard kill switch; `true` forces the no-op providers.                                                                                                                                                     |
| `OTEL_SERVICE_NAME`                                                   | _(binary name)_                  | `service.name` resource attribute (honoured by `bootstrap.Base`).                                                                                                                                                 |
| `OTEL_RESOURCE_ATTRIBUTES`                                            | _(unset)_                        | Extra `key=value` resource attributes (OTel standard).                                                                                                                                                            |
| `POD_NAME`, `POD_NAMESPACE`, `POD_IP`, `NODE_NAME`, `SERVICE_ACCOUNT` | _(unset)_                        | Mapped to `k8s.*` resource attributes when present (Kubernetes downward API).                                                                                                                                     |

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
| `vmafx-server` | `:8080` | Score requests, errors and latency; the quality family; ScoreStream sessions (open, ended by outcome, frames, duration); `vmafx_build_info`. |
| `vmafx-controller` | `:8080` | The server's request and quality families, plus the job queue: submitted, completed, failed, cancelled and requeued jobs, pending and running jobs and the age of the oldest pending job per tenant, queue wait and time to result, live nodes. |
| `vmafx-node` | `:9090` | Backend and vendor, slots, running jobs, jobs by backend and outcome, job run time, GPU memory per device, ScoreStream sessions. |

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

### Values read at scrape time

The controller's queue families and the node's GPU memory are read when
Prometheus scrapes, within 2 seconds. A read that fails leaves its own
families out of that scrape, serves the rest of the page, and counts in
`vmafx_metrics_read_errors_total{source}` (`queue`, `device_memory`). A node
reads GPU memory from `nvidia-smi` on a CUDA backend and from the amdgpu
sysfs files (`mem_info_vram_used`, `mem_info_vram_total`) on a HIP backend;
the CUDA image needs the driver utilities (`NVIDIA_DRIVER_CAPABILITIES`
including `utility`). The Intel `xe` driver and Metal expose no device memory
counter an unprivileged process can read: use the vendor exporter dashboards.

### Label cardinality

Every label is bounded. A label with a closed value set (`backend`,
`outcome`, `reason`, `vendor`, `profile`) maps any other value to `other`. An
open label reports at most a fixed number of distinct values per process,
`tenant` 256 and `model` 64, and merges every later value into `other`:
counts add up, the age of the oldest job takes the oldest, nothing is
dropped. An empty value (a request to `vmafx-server`, which has no tenants)
reads `none`. At these limits the largest family, `vmafx_quality_score`, is
bounded by 634 790 series per process; a deployment with a few tenants and
models serves a few hundred. The `profile` label of the quality family reads
`none` until scoring requests carry a profile.

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
kept as its negative fixture. The same test refuses an equality matcher on a
whole-number `le` or `quantile` value such as `le="70"`: Prometheus 3 stores
that bucket as `le="70.0"`, so the matcher selects nothing. The generated
queries select a bucket with `le=~"70(\\.0)?"` (`obsgen.LeMatcher`), which
matches the label as Prometheus 2 and Prometheus 3 store it.

The [dashboard tour](../observability/dashboards.md) describes each
dashboard for operators: the question every row answers and how to read it.
The SLO report, the usage and cost dashboard and the capacity dashboard read
the settings series of the rules (see
[Alerts and recording rules](#alerts-and-recording-rules)).

### Linting

Every generated dashboard passes Grafana's
[dashboard-linter](https://github.com/grafana/dashboard-linter) with
`--strict` and no exclusions; the go-ci workflow runs it on each change:

```bash
make lint-dashboards   # scripts/ci/lint-dashboards.sh: the pinned release, sha256-checked
```

The version and the checksum of its linux_amd64 release live in
`build-config.env` (`DASHBOARD_LINTER_VERSION`,
`DASHBOARD_LINTER_LINUX_AMD64_SHA256`); on another platform set
`DASHBOARD_LINTER` to a binary of that version.

## Alerts and recording rules

`deploy/prometheus/vmafx-rules.yaml` holds the recording rules and alerts,
generated from the same definitions as the dashboards
(`go run ./tools/obsgen -write`), with the default settings. Load it into
Prometheus as a rule file (`rule_files:` in `prometheus.yml`), or let the Helm
chart install it as a PrometheusRule
([monitoring on Kubernetes](../observability/kubernetes.md)). The thresholds
below are the defaults.

| Alert | Severity | Fires when | Runbook |
|-------|----------|------------|---------|
| `VMAFxComponentDown` | critical | a target that served `vmafx_build_info` in the last hour has been unscrapable for 5 minutes | [component down](../observability/runbooks/vmafx-component-down.md) |
| `VMAFxNoLiveNodes` | critical | jobs are pending and no node is registered, for 10 minutes | [no live nodes](../observability/runbooks/vmafx-no-live-nodes.md) |
| `VMAFxQueueAging` | warning | a tenant's oldest pending job is older than 30 minutes, for 15 minutes | [queue aging](../observability/runbooks/vmafx-queue-aging.md) |
| `VMAFxJobErrorBudgetBurn` | critical, warning | controller jobs fail fast enough to burn the 99 % objective's budget (14.4x over 1h and 5m; 6x over 6h and 30m) | [job error budget](../observability/runbooks/vmafx-job-error-budget-burn.md) |
| `VMAFxScoreErrorBudgetBurn` | critical, warning | the same for Score request errors | [score error budget](../observability/runbooks/vmafx-score-error-budget-burn.md) |
| `VMAFxScoreLatencyBudgetBurn` | critical, warning | the same for Score requests slower than 30 seconds | [score latency budget](../observability/runbooks/vmafx-score-latency-budget-burn.md) |
| `VMAFxScoreRegression` | warning | an hour's median score of a tenant and model is 5 points below the previous day's, with at least 20 scores, for an hour | [score regression](../observability/runbooks/vmafx-score-regression.md) |
| `VMAFxMetricsReadErrors` | warning | an instance fails to read its queue or GPU memory values for 15 minutes | [metrics read errors](../observability/runbooks/vmafx-metrics-read-errors.md) |

Every alert carries a `runbook_url` annotation pointing at its page under
[alert runbooks](../observability/runbooks/index.md). The recording rules
(`vmafx:job_failure_ratio:rate<window>`, `vmafx:score_error_ratio:...`,
`vmafx:score_slow_ratio:...` for each window of the burn rates, by default
1h, 5m, 6h and 30m; the settings series of the dashboards,
`vmafx:slo_objective{slo}`, `vmafx:slo_events:rate5m{slo}`,
`vmafx:slo_bad_events:rate5m{slo}`, `vmafx:price_job_second` and
`vmafx:price_job`, one sample per scraped VMAFx instance so a dashboard selects
them by job and instance;
`vmafx:quality_score:p50_1h`, `vmafx:quality_score:count_1h`) feed the
alerts and may be queried like any other series.

`deploy/prometheus/vmafx-rules.test.yaml` is the promtool unit test of the
rule file, generated next to it: every alert has a case where it fires and
one where it does not.

```bash
make check-prometheus-rules   # promtool check rules + promtool test rules, pinned release
```

The objectives (by default 99 % for jobs, Score errors and Score latency
within 30 seconds), the burn-rate windows and factors and the other alerts'
thresholds are settings ([ADR-2399](../adr/2399-observability-slo-settings-as-values.md)):
`monitoring.slo`, `monitoring.burnRates` and `monitoring.alerts` in the Helm
chart's values, whose PrometheusRule template `obsgen` generates from the
same rule code. A burn threshold is the PromQL expression
`(factor * (1 - objective))` of the values, so the chart and a rule file
rendered from the same values agree to the character. For Prometheus without
the operator, render the rule file from a values file:

```bash
go run ./tools/obsgen -render-rules -values monitoring.yaml -out vmafx-rules.yaml
```

`obsgen.DefaultSettings` (`pkg/observability/obsgen/settings.go`) holds the
defaults; `go run ./tools/obsgen -write` writes them into the block between
the `# BEGIN obsgen monitoring settings` and `# END obsgen monitoring
settings` markers of `deploy/helm/vmafx/values.yaml`. The chart's
`values.schema.json` and `Settings.Validate` refuse the same values
(`TestValidateAgreesWithTheChartSchema`), and
`scripts/ci/tests/test_helm_observability.py` checks that the chart rendered
with the defaults has the groups of the rule file, that a chart rendered with
an override has the groups `-render-rules` writes for it, and that promtool
accepts both.

## Compose example and smoke test

`deploy/compose/observability/` runs the three components with Prometheus,
the OpenTelemetry Collector, Tempo, Loki and Grafana
([Compose guide](../observability/compose.md)). Its `rules` service renders
the rule file from `monitoring-values.yaml` with
`go run ./tools/obsgen -render-rules`; Grafana reads the generated dashboards
and the generated data source provisioning
(`deploy/grafana/provisioning/datasources/vmafx.yaml`, `obsgen.PrometheusUID`,
`TempoUID`, `LokiUID`).

```bash
scripts/ci/observability-compose-smoke.sh --build   # images from the checkout, then the smoke test
make observability-compose-smoke                    # images already built
```

CI runs it in the non-required workflow _Observability Smoke_
(`.github/workflows/observability-compose.yml`): nightly, on dispatch, and on
a ready pull request labelled `run-observability-smoke`, through the
`observability` job of `.github/workflows/pr-label-lanes.yml`.

The smoke test (`tools/obssmoke`) runs in the Compose network. It reads every
dashboard through `obsgen.DashboardQueries`, the parser the dashboard check
uses, sets every variable to "All" and the time-range variables to `10m`, and
requires data from each Prometheus query after its traffic; the queries that
cannot have data on a CPU-only stack are listed with their reason in
`tools/obssmoke/exemptions.go`, and an exempted query that returns data fails
the run. `obsgen -write` writes world-readable files (`0644`), because the
example mounts them into containers that run as other users.

## Logs

Logs stay on the golusoris slog stream (stderr for `vmafx-mcp` on stdio,
stdout otherwise). The OTLP log exporter is initialised alongside traces
and metrics but no slog → OTel bridge is wired (ADR-0927 Phase 3), so no
log records are exported; set `VMAFX_OTEL_EXPORT_LOGS=false` to skip the
exporter entirely if the collector has no logs pipeline.

Request-scoped server log lines share one field set (`request_id`, `rpc`,
`route`, `model`, `backend`, `duration_s`, `error`); `request_id` is the trace
id when the request has a span, so a log line and its trace join on one value.
The field table is in [gRPC service: Logging](../server/grpc.md#logging).

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
