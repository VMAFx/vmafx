- **Observability stack with Docker Compose (RC4, ADR-2349, ADR-2399,
  #2430).** `deploy/compose/observability/` runs `vmafx-server`,
  `vmafx-controller` and a CPU `vmafx-node` with Prometheus (the generated
  rules, rendered at start from `monitoring-values.yaml`, the Helm chart's
  `monitoring.slo` / `burnRates` / `alerts` keys), an OpenTelemetry Collector,
  Tempo, Loki and Grafana (the generated dashboards and linked data sources).
  The VMAFx images build from the checkout on the first `up`.
  `scripts/ci/observability-compose-smoke.sh` (`make
  observability-compose-smoke`) sends traffic and checks every component is
  scraped, the rules are healthy, every dashboard query returns data, Grafana
  is provisioned and traces reach Tempo. See
  [the guide](docs/observability/compose.md).
