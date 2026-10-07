<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-2349: One metric definition drives the services, the generated dashboards and the observability package

- **Status**: Accepted
- **Date**: 2026-10-07
- **Deciders**: maintainer (decisions Q-109, Q-110, Q-111; RC4 work package 16 brief)
- **Tags**: observability, go, grafana, prometheus, helm, rc4, fork-local

## Context

The platform's monitoring was partly broken. Prometheus `/metrics` was served
only by `vmafx-server` and `vmafx-controller`; the node, which runs the work,
served none. The one shipped dashboard, `deploy/grafana/vmafx-overview.json`,
was written by hand, and five of its seven queries named series nothing
emits: three stats used names the controller never had
(`vmafx_controller_jobs_queued`, `vmafx_jobs_in_flight`,
`vmafx_controller_nodes_active`), and two panels queried OpenTelemetry
instruments that no binary registers (`vmafx_frames_per_second`,
`vmafx_gpu_utilization`). Metric names lived in three places (the services,
the dashboard, the docs) and nothing compared them. Logs had no OTLP export,
and there were no alerts.

The maintainer decided the scope (issue #2430): metrics on every long-running
binary including a quality family, a dashboard set (Overview, Quality, Nodes
and devices, Live sessions, an SLO report, per-tenant usage and cost,
capacity forecasting), alerts and recording rules with runbooks, Helm and
Docker Compose packaging (Q-109); metrics, traces with exemplars and logs
through an slog-to-OTLP bridge into Loki with trace-to-log links (Q-110); and
dashboards generated in Go with the Grafana Foundation SDK from the same
metric definitions the services use, linted with Grafana's dashboard-linter,
with a test that fails when a panel queries a metric nothing emits (Q-111).

## Decision

We will define every Prometheus family once, as Go data in
`pkg/observability/metricdef`: name, type, unit, help, labels with their
cardinality bound, histogram buckets, and the binaries that emit it. The
services build their collectors only through `pkg/observability`'s handles
(`NewCounter`, `NewGauge`, `NewHistogram`, `RegisterScraped`), which bound
every label value: a closed label maps an unknown value to `other`, an open
label reports at most its limit of distinct values per process (`tenant` 32,
`model` 16) and merges later ones into `other`, never dropping a series. A
test per binary fails when its `/metrics` page and the definition disagree.
`pkg/observability/obsgen` generates the dashboards with the Grafana
Foundation SDK (pinned `v0.0.20`) and the metric reference page from the same
definition; `go run ./tools/obsgen -write` writes them, a test fails on a
stale copy, and `CheckDashboard` fails any shipped panel, annotation or
variable that names a series outside the definition and a short, tested
allow-list (`up`, the client library's Go and process series). The node
serves `/metrics` and the statuspage probes on an HTTP listener
(`VMAFX_HTTP_ADDR`, default `:9090`, the chart's existing `node.metricsPort`).
The controller's queue families are read from the queue when Prometheus
scrapes, and its job counters carry the tenant. The rest of the package
follows the same rule, one source and generated artefacts: recording rules
and alerts with `promtool` tests and one runbook per alert, the slog-to-OTLP
bridge (in golusoris when the gap is there), Helm and Compose packaging, and
the SLO, usage-and-cost and capacity dashboards.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Go data in `metricdef`, read by services and generator (chosen) | Both consumers are Go and import it directly; no generation step between definition and services; compile-time references from dashboards to families | A non-Go consumer would need an export step | Every consumer today is Go or generated from Go |
| TOML definition with a standard-library Python generator, as the RC4 C API does (ADR-1852) | Same tooling as the API definition; language-neutral | Two hops (TOML to Go, Go to dashboards); generated Go committed next to hand-written Go; the dashboard generator must be Go anyway (Q-111) | Adds a generator and a drift surface without a consumer that needs it |
| Hand-written dashboard JSON plus a lint and a name check | No generator | The dashboards drift from the services again; Q-111 asks for generation | Rejected by Q-111 |
| Jsonnet / grafonnet | Mature dashboard library | Second language, not the Foundation SDK Q-111 names | Rejected by Q-111 |
| OpenTelemetry metrics SDK with a Prometheus exporter instead of `client_golang` | One metrics API for OTLP and Prometheus | The production path is the Prometheus scrape; the OTel instruments have had no producer since ADR-0782; a second migration in the same change | Out of scope; the definition can drive either API later |
| Drop a label past its limit, or reject the value | Simpler | A dropped label loses the data Q-109 asks for; a rejected value hides work | Overflow into `other` keeps every count and bounds the series |
| Unlabelled controller totals next to per-tenant families | Old queries keep their exact series | Two families for one fact (HISS-19) | `sum()` over the tenant label gives the total |
| Serve the node's metrics on the gRPC port or push them to the controller | No second listener | Prometheus scrapes HTTP; a push path is a second transport to secure | The chart already reserves `node.metricsPort` |

## Consequences

- **Positive**: one place to add a metric; dashboards cannot query a name
  nothing emits; every panel passes dashboard-linter without exclusions; the
  node is observable; label cardinality is bounded and documented per family.
- **Negative**: the controller's job counters and queue gauges gain a
  `tenant` label, so a query that read the bare series sees one series per
  tenant until it is wrapped in `sum()`; `pkg/observability.NewMetrics`
  returns an error and `SetControllerSources` is gone (ADR-1014's isolation
  property is kept: every family registers on the service's own registry);
  the queue's `Cancel` and `ReportResult` report whether they moved the job,
  so a repeated report is counted once.
- **Neutral / follow-ups**: the remaining work packages of issue #2430
  (dashboard-linter in CI and the other dashboards, rules and runbooks, logs,
  packaging, SLO / cost / capacity) build on this definition. The quality
  family's `profile` label waits for a scoring profile in the request.

## Supply-chain impact

- **New dependencies**: `github.com/grafana/grafana-foundation-sdk/go`
  `v0.0.20` (Apache-2.0, no transitive requirements), build-time only: it is
  imported by the generator package and its test, not by any service binary.
- **Removed dependencies**: none.
- **Build-time fetches**: none beyond the Go module.

## References

- Decisions Q-109, Q-110 and Q-111, maintainer, 2026-10-07; issue #2430.
- [ADR-0782](0782-otel-tracing.md): span, attribute and metric schema.
- [ADR-0927](0927-opentelemetry-traces-metrics-phase1.md): OpenTelemetry rollout.
- [ADR-1014](1014-r5-prometheus-registry.md): every family on the service's own registry.
- [ADR-1119](1119-golusoris-go-framework-adoption.md): golusoris composition, `bootstrap.HTTP`.
- [ADR-1852](1852-vmafx-api-redesign.md): the RC4 definition-and-generator approach this ADR compares against.
- Grafana Foundation SDK: <https://github.com/grafana/grafana-foundation-sdk> (Go module tag `go/v0.0.20`).
