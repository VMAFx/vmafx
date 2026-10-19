# Monitoring on Kubernetes

The Helm chart (`deploy/helm/vmafx`) ships everything Prometheus and Grafana
need to watch a VMAFx release: a scrape target per component, the alert and
recording rules with your SLO objectives, and the dashboards. All of it is off
until you set `monitoring.enabled`.

## Before you start

- The [Prometheus operator](https://github.com/prometheus-operator/prometheus-operator)
  CRDs (`ServiceMonitor`, `PodMonitor`, `PrometheusRule`) are installed, for
  example by kube-prometheus-stack. Without them `helm install` fails on the
  unknown kinds; keep `monitoring.enabled: false` in that case and load the
  rule file yourself ([without the operator](#without-the-prometheus-operator)).
- For the dashboards, Grafana runs the dashboard sidecar, which loads every
  ConfigMap carrying a label (kube-prometheus-stack:
  `grafana.sidecar.dashboards.enabled`, label `grafana_dashboard`).

## Turn it on

```yaml
monitoring:
  enabled: true
  serviceMonitor:
    labels:
      release: kube-prometheus-stack   # your Prometheus's serviceMonitorSelector
  prometheusRule:
    labels:
      release: kube-prometheus-stack   # your Prometheus's ruleSelector
```

```bash
helm upgrade --install vmafx deploy/helm/vmafx -f monitoring.yaml
```

That renders:

| Resource | Name | What it does |
| --- | --- | --- |
| `ServiceMonitor` | `<release>-vmafx` | scrapes the server's `/metrics` on its `http` port |
| `ServiceMonitor` | `<release>-vmafx-controller` | the controller's `/metrics` (with `controller.enabled`) |
| `ServiceMonitor` | `<release>-vmafx-node` | the nodes' `/metrics` on `node.metricsPort` (with `node.enabled`) |
| `PodMonitor` | `<release>-vmafx-operator` | the operator's pods on port 8080; it has no Service (with `operator.enabled`) |
| `PrometheusRule` | `<release>-vmafx` | the alerts and recording rules of `deploy/prometheus/vmafx-rules.yaml` |
| `ConfigMap` | `<release>-vmafx-dashboard-<name>` | one per dashboard: Overview, Quality, Nodes and devices, Live sessions, SLO report, Usage and cost, Capacity |
| `NetworkPolicy` | `<release>-vmafx-allow-metrics-scrape` | with `networkPolicy.enabled`: Prometheus may reach every metrics port |

Check it:

```bash
kubectl get servicemonitor,podmonitor,prometheusrule -l app.kubernetes.io/instance=vmafx
kubectl get configmap -l grafana_dashboard=1
```

In Prometheus, _Status > Targets_ lists one target per pod of each component,
and `vmafx_build_info` returns one series per pod.

## Choose what is scraped

`monitoring.components` switches the monitors one by one; a component that
is not deployed gets none whatever the switch says.

```yaml
monitoring:
  enabled: true
  components:
    server: true
    controller: true
    node: true
    operator: false   # for example when another stack scrapes the operator
```

`monitoring.serviceMonitor` holds the scrape settings every monitor shares:
`interval`, `scrapeTimeout`, `path`, `scheme`, `honorLabels`, extra `labels`
and `namespace`. A monitor placed in another namespace (your Prometheus's)
selects the release namespace by itself.

## Set the objectives and thresholds

The alerts read their objectives, burn-rate windows and thresholds from
`monitoring.slo`, `monitoring.burnRates` and `monitoring.alerts`. The defaults
in the chart's `values.yaml` are generated from the same code as the rule file;
override any key in your own values file and the PrometheusRule is rendered
with it:

```yaml
monitoring:
  enabled: true
  slo:
    jobSuccess: 0.995          # 99.5 % of controller jobs complete
    scoreLatency: 0.99         # 99 % of Score requests finish within ...
    scoreLatencySeconds: "60"  # ... 60 seconds
  burnRates:
    slow:
      longWindow: 1d           # a slower warning: one day and its last 2 hours
      shortWindow: 2h
      factor: 3
  alerts:
    queueAgeSeconds: 3600      # page when a tenant's oldest job waits an hour
```

| Key | Default | Meaning |
| --- | --- | --- |
| `slo.jobSuccess` | `0.99` | share of controller jobs that complete rather than fail, over 30 days |
| `slo.scoreSuccess` | `0.99` | share of Score requests that return no error |
| `slo.scoreLatency` | `0.99` | share of Score requests that finish within `scoreLatencySeconds` |
| `slo.scoreLatencySeconds` | `"30"` | a bucket bound of `vmafx_server_score_duration_seconds`, as a string |
| `burnRates.fast` | `1h` / `5m`, `14.4`, for `2m` | critical: both windows burn the budget `factor` times too fast |
| `burnRates.slow` | `6h` / `30m`, `6`, for `15m` | warning, the same with a lower factor |
| `alerts.queueAgeSeconds` | `1800` | `VMAFxQueueAging`: how old a tenant's oldest pending job may get |
| `alerts.scoreRegressionPoints` | `5` | `VMAFxScoreRegression`: the drop of an hour's median score below the previous day's |
| `alerts.scoreRegressionMinScores` | `20` | `VMAFxScoreRegression`: scores an hour needs to be compared |
| `cost.perJobSecond` | `0` (unset) | price of one second of job run time, for the Usage and cost dashboard |
| `cost.perJob` | `0` (unset) | price of one completed or failed job; cancelled jobs are not charged |
| `cost.currency` | `""` | ISO 4217 code shown with the costs |

The prices are yours: VMAFx assumes none, and an unset price leaves its cost
panels empty. One price per job-second and one per job apply to every backend.

A burn rule fires when the ratio of bad events over its long window and over
its short window both exceed `factor * (1 - objective)`; the rule states that
product as PromQL, so the alert shows the values you set. The chart's schema
refuses a value the rules cannot use: an objective of 0 or 1 or outside that
range, a latency bound that is no bucket of the histogram (the error lists the
allowed ones), a window that is no Prometheus duration such as `5m` or
`1h30m`, a factor or threshold that is not positive. Pass decimals through a
values file or `--set-json`; `--set` turns `0.995` into a string, which the
schema refuses.

Each alert links its [runbook](runbooks/index.md).

## Dashboards

`monitoring.dashboards` controls the ConfigMaps:

```yaml
monitoring:
  dashboards:
    namespace: monitoring          # where the Grafana sidecar looks; default: release namespace
    labels:
      grafana_dashboard: "1"       # the label your sidecar selects
    annotations:
      grafana_folder: VMAFx        # when the sidecar takes folders from an annotation
    gpuExporters:
      dcgm: true                   # NVIDIA DCGM exporter dashboard
      amd: false                   # AMD device metrics exporter
      intel: false                 # Intel XPU Manager
```

The dashboards select their Prometheus data source with a variable, so they
work with whichever Prometheus data source Grafana has. A vendor GPU
dashboard shows data only where that vendor's exporter runs; ship it only for
those clusters.

## Network policies

With `networkPolicy.enabled`, the chart denies ingress it does not list.
`networkPolicy.allow.metricsScrape` (on by default while monitoring is on)
admits the scrape on every component's metrics port: the server's and the
controller's HTTP port, the node's metrics port and the operator's 8080. Its
empty selectors admit pods of the release namespace only. A Prometheus in
another namespace needs that namespace named:

```yaml
networkPolicy:
  enabled: true
  allow:
    metricsScrape:
      fromNamespaceSelector:
        kubernetes.io/metadata.name: monitoring
      fromPodSelector:
        app.kubernetes.io/name: prometheus
```

## Without the Prometheus operator

Leave `monitoring.enabled` off and load the rule file into Prometheus as a
`rule_files:` entry. `deploy/prometheus/vmafx-rules.yaml` holds the defaults;
for your own settings, render it from the same values file you would give the
chart:

```bash
go run ./tools/obsgen -render-rules -values monitoring.yaml -out vmafx-rules.yaml
promtool check rules vmafx-rules.yaml
```

The file has the same groups as the PrometheusRule the chart renders from
`monitoring.yaml`. Import the dashboards from `deploy/grafana/dashboards/`.

## Reference

- [Observability stack with Docker Compose](compose.md): the same rules,
  dashboards and settings on one machine, with Tempo and Loki.
- [Metric reference](metrics.md): every series the components serve.
- [Observability](../development/observability.md): how the metrics, the
  dashboards and the rules are generated, and the alert table.
- [ADR-2399](../adr/2399-observability-slo-settings-as-values.md): why the
  settings are chart values and the thresholds PromQL.
