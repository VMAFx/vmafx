## Observability: chart monitoring generated from the rule code (2026-10-07)

`rc4/obs-5-packaging`, [ADR-2399](adr/2399-observability-slo-settings-as-values.md). Fork-only. `deploy/helm/vmafx/templates/prometheusrule.yaml`,
`deploy/helm/vmafx/files/dashboards/*.json` and the block between `# BEGIN obsgen monitoring settings` and `# END obsgen monitoring settings`
in `deploy/helm/vmafx/values.yaml` are written by `go run ./tools/obsgen -write`: on a conflict take either side and regenerate, never
hand-merge. `TestGeneratedFilesAreCurrent`, `TestValidateAgreesWithTheChartSchema` and `scripts/ci/tests/test_helm_observability.py`
guard them. A rule must match a histogram bucket with `obsgen.LeMatcher`, never `le="<whole number>"` (Prometheus 3 stores `le="30.0"`).
