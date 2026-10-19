- **Monitoring in the Helm chart: a monitor per component, the alert rules
  with your SLOs, the dashboards (RC4, ADR-2399, #2430).** With
  `monitoring.enabled` the chart renders a ServiceMonitor for the server,
  the controller and the nodes and a PodMonitor for the operator
  (`monitoring.components` switches each), a PrometheusRule with the
  generated alerts and recording rules, and one ConfigMap per Grafana
  dashboard for the dashboard sidecar (`monitoring.dashboards`, vendor GPU
  dashboards opt-in). The SLO objectives, burn-rate windows and factors and
  the queue-age and score-regression thresholds are values
  (`monitoring.slo`, `monitoring.burnRates`, `monitoring.alerts`), checked by
  the chart's schema; `go run ./tools/obsgen -render-rules -values <file>`
  writes the same rules as a plain rule file. With `networkPolicy.enabled`,
  `networkPolicy.allow.metricsScrape` admits the scrape. The burn-rate alerts
  state their threshold as `(factor * (1 - objective))`. See
  [monitoring on Kubernetes](docs/observability/kubernetes.md).
