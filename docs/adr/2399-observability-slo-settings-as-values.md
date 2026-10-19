<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-2399: SLO objectives, burn-rate windows and alert thresholds are chart values, rendered from one rule builder

- **Status**: Accepted
- **Date**: 2026-10-07
- **Deciders**: maintainer (decision Q-165; RC4 work package 16 brief)
- **Tags**: observability, prometheus, helm, rc4, fork-local

## Context

[ADR-2349](2349-observability-package.md) generates the alert and recording
rules (`deploy/prometheus/vmafx-rules.yaml`) from Go code with the SLO
objectives (99 % for jobs, Score errors and Score latency within 30 seconds),
the burn-rate windows and factors and the queue-age and score-regression
thresholds fixed as constants. An operator who needs another objective had to
edit the generated file or the generator. The packaging step of the work
package (Helm chart, Docker Compose example) has to ship the rules in a form
each deployment can tune, and the two packagings must not drift apart from
each other or from the committed rule file.

## Decision

We will make the objectives, the two burn rates (long window, short window,
factor, `for`) and the other alerts' thresholds settings
(`obsgen.Settings`): the Helm values `monitoring.slo`, `monitoring.burnRates`
and `monitoring.alerts`, applied when the chart renders its PrometheusRule,
and the same keys in one values file for the Compose example. The rules are
built once, from a parameter set that `obsgen` renders two ways: with the
values of a `Settings` for a rule file (`RenderRules`, the committed default
file, and `go run ./tools/obsgen -render-rules -values <file>`), and with Helm
expressions over `$.Values.monitoring` for the chart's generated
`templates/prometheusrule.yaml`. A burn threshold is the PromQL expression
`(factor * (1 - objective))` of the values, and numbers are printed as Helm
prints them, so both renderings of the same values are equal to the
character. The defaults live in `obsgen.DefaultSettings`; the generator writes
them into a marked block of the chart's `values.yaml`. The chart's
`values.schema.json` and `Settings.Validate` refuse the same values. The
chart's dashboards ship as ConfigMaps for the Grafana sidecar from copies
under `files/dashboards/` that the generator keeps identical to
`deploy/grafana/dashboards/`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Thresholds stay generator constants; operators patch the rule file | One rendering, nothing to keep in step | Every tuned deployment carries a patch that each regeneration overwrites | The maintainer chose tunable values (Q-165) |
| Hand-written Helm template next to the generated rule file | Plain Helm, no generator change | Two copies of every rule; the drift between them is unchecked | Duplicates the rules the generator already builds (HISS-19) |
| Helm computes the thresholds (`mulf`, `subf`, `round`) | Rule shows a single number | Sprig's decimal arithmetic and Go's float formatting differ in the last digits, so chart and rule file disagree for some values | The PromQL expression keeps both renderings identical and shows the operator's own values |
| Compose renders its rules with `helm template` | One renderer | Compose would need Helm and a PrometheusRule-to-rule-file extraction step | `obsgen -render-rules` writes the rule file directly from the same rule code |

## Consequences

- **Positive**: an operator sets an objective or a threshold in a values file
  and the chart, a rule file rendered for Prometheus without the operator and
  the Compose example all apply it; a test proves the chart with the defaults
  renders the committed rule file and with an override renders what
  `-render-rules` writes for it, and promtool checks both.
- **Negative**: the alert expressions carry `(14.4 * (1 - 0.99))` rather than
  `0.144`; the queue-age annotation states seconds rather than minutes.
  `--set` turns a decimal into a string, which the schema refuses: decimals go
  through a values file or `--set-json`.
- **Neutral / follow-ups**: the recording-rule windows are the distinct
  windows of the two burn rates, in the order of the values; the Compose
  example (next work-package step) reads the same keys.

## Supply-chain impact

- **New dependencies**: none. `github.com/santhosh-tekuri/jsonschema/v6`
  (already in the module graph through `kin-openapi`) becomes a direct
  test dependency of `pkg/observability/obsgen`, which validates values
  against the chart's schema in `TestValidateAgreesWithTheChartSchema`.
- **Removed dependencies**: none.
- **Build-time fetches**: none.

## References

- Decision Q-165, maintainer, 2026-10-07: "SLO objectives, windows and alert thresholds become Helm values applied when the chart renders its PrometheusRule (generated defaults, values schema-checked), and the Compose example reads the same values from one file. Do it in PR 5 together with the chart files/ drift-checked copies."
- [ADR-2349](2349-observability-package.md): the metric definition and the generated rules.
- Issue #2430 (RC4 work package 16).
- Multi-window, multi-burn-rate alerting: Google SRE Workbook, "Alerting on SLOs" (<https://sre.google/workbook/alerting-on-slos/>).
