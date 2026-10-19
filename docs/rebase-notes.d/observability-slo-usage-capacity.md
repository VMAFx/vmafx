## Observability: SLO report, usage and cost, capacity (2026-10-08)

`rc4/obs-6-slo-cost-capacity`, [ADR-2349](adr/2349-observability-package.md), #2430. Fork-only. The rule file, the chart's
PrometheusRule template and values block, and the three dashboards are generated (`go run ./tools/obsgen -write`): regenerate, never
hand-merge. The settings series (`vmafx:slo_objective`, `vmafx:slo_events:rate5m`, `vmafx:slo_bad_events:rate5m`,
`vmafx:price_job_second`, `vmafx:price_job`) keep `job` and `instance` labels, because dashboard-linter requires both matchers on
every query; a price rule keeps only a positive price.
