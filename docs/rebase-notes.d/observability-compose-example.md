## Observability: Compose example and smoke test (2026-10-08)

`rc4/obs-5b-compose`, [ADR-2399](adr/2399-observability-slo-settings-as-values.md), #2430. Fork-only.
`deploy/grafana/provisioning/datasources/vmafx.yaml` is generated (`go run ./tools/obsgen -write`): on a conflict take either side and
regenerate. Its URLs are the service names of `deploy/compose/observability/compose.yaml`; renaming a service there changes
`obsgen/datasources.go` too. `tools/obssmoke` reads dashboards through `obsgen.DashboardQueries`, the parser `CheckDashboard` uses.
