- **Observability: one metric definition, node `/metrics`, queue and quality
  metrics, a generated Overview dashboard (RC4, ADR-2349, #2430).** Every
  Prometheus family the services serve is defined once in
  `pkg/observability/metricdef`, with its labels and their cardinality bound;
  the services build their collectors from it, and
  [the metric reference](docs/observability/metrics.md) is generated from it.
  `vmafx-node` serves `/metrics`, `/livez`, `/readyz` and `/startupz` on
  `VMAFX_HTTP_ADDR` (default `:9090`): backend and vendor, slots, running
  jobs, jobs by backend and outcome, job run time. The controller adds
  cancelled and requeued jobs (by reason), the age of each tenant's oldest
  pending job, queue wait and time to result; the server and controller add
  the quality family `vmafx_quality_score` per tenant and model; every
  component serves `vmafx_build_info`. The Overview dashboard is generated
  with the Grafana Foundation SDK (`go run ./tools/obsgen -write`) under
  `deploy/grafana/dashboards/`; it replaces `deploy/grafana/vmafx-overview.json`,
  five of whose seven queries named series nothing emits, and a test fails
  any shipped panel that queries a series nothing emits. See
  [observability](docs/development/observability.md#metrics).
