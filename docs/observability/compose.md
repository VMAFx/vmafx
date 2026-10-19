# Observability stack with Docker Compose

`deploy/compose/observability/` runs VMAFx and everything that watches it on
one machine: `vmafx-server`, `vmafx-controller` and a CPU `vmafx-node`,
Prometheus with the generated alert and recording rules, an OpenTelemetry
Collector, Tempo for traces, Loki for logs, and Grafana with the generated
dashboards. Use it to try the dashboards and alerts, or as the template of a
single-host deployment.

## Start it

From the repository root:

```bash
docker compose -f deploy/compose/observability/compose.yaml up -d
```

The first `up` builds the three VMAFx images from your checkout
(`local/vmafx-server:dev` and so on); the node image compiles FFmpeg, which
takes a while once. To run published images instead:

```bash
VMAFX_IMAGE_REGISTRY=ghcr.io/vmafx VMAFX_TAG=v1.0.0 \
  docker compose -f deploy/compose/observability/compose.yaml up -d --no-build
```

| Service | Address on your machine | What it is |
| --- | --- | --- |
| Grafana | <http://127.0.0.1:3000> (`admin` / `admin`) | dashboards in folder _VMAFx_, data sources Prometheus, Tempo, Loki |
| Prometheus | <http://127.0.0.1:9090> | scrapes every component every 5 s; _Alerts_ lists the generated rules |
| vmafx-server | <http://127.0.0.1:8080> | `POST /v1/score`, Swagger UI at `/swagger` |
| vmafx-controller | `127.0.0.1:9091` (gRPC) | `SubmitJob`, `GetJob`, `CancelJob`; auth is off, every call is tenant `dev` |

The other services (the collector, Tempo, Loki, the node) are reachable only
inside the Compose network. `PROMETHEUS_PORT`, `GRAFANA_PORT`,
`VMAFX_SERVER_PORT` and `VMAFX_CONTROLLER_GRPC_PORT` move the published ports;
`GRAFANA_ADMIN_PASSWORD` sets Grafana's admin password.

## Score something

Put a reference and a distorted video into `deploy/compose/observability/media/`
(or point `VMAFX_MEDIA_DIR` at a directory). It is mounted at `/media` in the
three VMAFx containers and is the only scoring root, so paths start with
`/media/`. The Score API takes no frame geometry: use Y4M or a container
FFmpeg reads.

```bash
curl -s -X POST http://127.0.0.1:8080/v1/score \
     -H 'Content-Type: application/json' \
     -d '{"reference":"/media/ref.y4m","distorted":"/media/dis.y4m"}'
```

A job through the controller, which hands it to the node:

```bash
grpcurl -plaintext -d '{"scoring":{"reference":"/media/ref.y4m","distorted":"/media/dis.y4m"}}' \
    127.0.0.1:9091 vmafx.controller.v1.VmafxController/SubmitJob
```

Within a few seconds the _VMAFx Overview_ dashboard shows the request, the
job and its queue wait, _VMAFx Quality_ the score, and _VMAFx Nodes and
devices_ the node's slots and run time. Grafana's _Explore_ finds the
request's trace in Tempo (`service.name` `vmafx-server`).

## Change the alert settings

`monitoring-values.yaml` holds the SLO objectives, burn-rate windows, alert
thresholds and the prices of the usage and cost dashboard, in the same keys as
the Helm chart's values
([the table](kubernetes.md#set-the-objectives-and-thresholds)); keys left out
keep their defaults. When the stack starts, the `rules` service renders
Prometheus's rule file from it with `go run ./tools/obsgen -render-rules`,
which writes the same rules the chart's PrometheusRule holds for the same
values. After an edit:

```bash
docker compose -f deploy/compose/observability/compose.yaml up -d --force-recreate rules prometheus
```

A value the rules cannot use (an objective of 1, a latency bound that is no
bucket of the histogram, a window that is no Prometheus duration) stops the
`rules` service with the reason in its log, and Prometheus does not start:
`docker compose -f deploy/compose/observability/compose.yaml logs rules`.

## How the signals connect

- **Metrics**: Prometheus scrapes `/metrics` of the server and the controller
  on their HTTP port and of the node on `VMAFX_HTTP_ADDR` (`:9090`). The
  components' own OTLP metrics are off (`VMAFX_OTEL_EXPORT_METRICS=false`):
  the dashboards read the scraped families.
- **Traces**: every component sends spans to the collector
  (`OTEL_EXPORTER_OTLP_ENDPOINT=otel-collector:4317`), which forwards them to
  Tempo. Prometheus keeps exemplars (`--enable-feature=exemplar-storage`) and
  the Prometheus data source links an exemplar's `trace_id` to Tempo, for the
  latency histograms that carry exemplars (the logs-and-traces step of #2430).
- **Logs**: the collector forwards OTLP logs to Loki's `/otlp` endpoint,
  where `service_name` is a label and `trace_id`, `span_id` are structured
  metadata; Tempo links a span to its logs and Loki a log line to its trace.
  The components send their logs once the slog-to-OTLP bridge lands (the
  logs step of #2430).

The data source provisioning is generated (`deploy/grafana/provisioning/`,
`go run ./tools/obsgen -write`); its URLs are this file's service names.

## Smoke test

`scripts/ci/observability-compose-smoke.sh` starts the stack on ephemeral
ports with the 576x324 pair of `testdata/` as media, and runs
`tools/obssmoke` inside the network, with test prices in `XTS` (the ISO 4217
code reserved for testing) so the cost panels have data. It sends Score
requests that pass and
fail to the server and the controller, a ScoreStream session, and controller
jobs that complete, fail and are cancelled; then it checks that Prometheus
scrapes all three components, that the rendered rules evaluate without an
error, that every Prometheus query of every dashboard returns data, that
Grafana provisioned every dashboard and data source, and that traces of the
server and the controller reached Tempo. It waits 150 seconds after the
traffic, so the capacity forecasts have two 1-minute points to fit. A query
that cannot return data in this stack (it needs a GPU) is listed with its
reason in
`tools/obssmoke/exemptions.go`, and an exempted query that does return data
fails the run.

```bash
scripts/ci/observability-compose-smoke.sh --build   # build the images from the checkout first
make observability-compose-smoke                    # the same with images already built
```

In CI the workflow _Observability Smoke_
(`.github/workflows/observability-compose.yml`) runs it with `--build`
nightly, on manual dispatch, and on a ready pull request that carries the
label `run-observability-smoke` (called from
`.github/workflows/pr-label-lanes.yml`). It is not a required check: add the label
to a pull request that changes the metrics, the dashboards, the rules or the
Compose files.

## Stop it

```bash
docker compose -f deploy/compose/observability/compose.yaml down      # keep the data volumes
docker compose -f deploy/compose/observability/compose.yaml down -v   # and drop them
```
