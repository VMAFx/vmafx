# Observability

VMAFx reports what it does through three signals and ships everything to
watch them with. This guide is for operators who run the server, the
controller and the nodes; the [developer guide](../development/observability.md)
covers how the telemetry is wired and generated.

| What you get | Where it comes from |
| --- | --- |
| Prometheus metrics from every long-running component | `/metrics` of `vmafx-server`, `vmafx-controller`, `vmafx-node` (and the operator's controller-runtime metrics); [metric reference](metrics.md) |
| Ten Grafana dashboards | `deploy/grafana/dashboards/`; [dashboard tour](dashboards.md) |
| Eight alerts with a runbook each, and the recording rules they read | `deploy/prometheus/vmafx-rules.yaml`; [alert runbooks](runbooks/index.md) |
| Traces | OpenTelemetry over OTLP/gRPC to your collector; [OpenTelemetry](otel.md) |
| Settings: SLO objectives, burn rates, alert thresholds, prices | Helm values `monitoring.slo`, `.burnRates`, `.alerts`, `.cost`, or the same keys in one file for Compose |

## Choose how to install it

- **Kubernetes with the Prometheus operator**: set `monitoring.enabled` in
  the Helm chart. It renders a ServiceMonitor or PodMonitor per component, a
  PrometheusRule with your settings and a ConfigMap per dashboard for the
  Grafana sidecar. See [monitoring on Kubernetes](kubernetes.md).
- **One machine, or to try it out**: the Docker Compose stack runs VMAFx with
  Prometheus, an OpenTelemetry Collector, Tempo, Loki and Grafana, already
  wired together. See [the Compose stack](compose.md).
- **Your own Prometheus and Grafana**: scrape the three components'
  `/metrics`, load the rule file (render your own with
  `go run ./tools/obsgen -render-rules -values <file>`), and import the
  dashboards' JSON. See
  [without the Prometheus operator](kubernetes.md#without-the-prometheus-operator).

## The first hour

1. **Check the scrape.** In Prometheus, `vmafx_build_info` returns one series
   per server, controller and node pod; _Status > Targets_ shows them up.
2. **Open the Overview.** _Components up_ matches your pods, _Live nodes_
   matches your nodes, and the queue is empty or draining
   ([tour](dashboards.md#overview)).
3. **Check the rules.** _Alerts_ in Prometheus lists the eight VMAFx alerts,
   none of them in error. Each alert's `runbook_url` opens its runbook.
4. **Set your objectives.** The defaults are 99 % for job success, Score
   request success and Score requests within 30 seconds. Change them, the
   burn-rate windows and the thresholds in `monitoring.slo`, `.burnRates` and
   `.alerts`; the chart's schema refuses a value the rules cannot use
   ([settings](kubernetes.md#set-the-objectives-and-thresholds)).
5. **Set prices, if you account usage.** `monitoring.cost.perJobSecond` and
   `monitoring.cost.perJob`, in `monitoring.cost.currency`, fill the cost
   panels of the [usage and cost dashboard](dashboards.md#usage-and-cost).
   VMAFx assumes no price: without them the cost panels stay empty.
6. **Point the traces somewhere.** Set `VMAFX_OTEL_ENDPOINT=collector:4317`
   (or `OTEL_EXPORTER_OTLP_ENDPOINT=http://collector:4317`) on the components
   ([OpenTelemetry](otel.md)).

## When an alert fires

Every alert links its runbook: what it means, what it costs, how to find the
cause and how to fix it. The [runbook index](runbooks/index.md) lists them;
the [dashboard tour](dashboards.md) says which dashboard each one starts from.

## Design and decisions

- [ADR-2349](../adr/2349-observability-package.md): one metric definition
  drives the services, the generated dashboards and the rules.
- [ADR-2399](../adr/2399-observability-slo-settings-as-values.md): the SLO
  objectives, burn rates and thresholds are Helm values, rendered from one
  rule builder.
