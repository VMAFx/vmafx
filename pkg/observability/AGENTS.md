# AGENTS.md — pkg/observability

## Package role

`pkg/observability` is shared telemetry surface for every VMAFX Go
service. Provides:

- `NewLogger(level)` — `log/slog` JSON logger.
- `metricdef/` — one definition of every Prometheus family (name, kind,
  unit, labels + cardinality bound, buckets, emitting binaries). Data only,
  stdlib only.
- `NewRegistry()` (Go + process collectors, `vmafx_build_info`),
  `NewMetrics(reg)` (scoring + quality families), `NewCounter` /
  `NewGauge` / `NewHistogram` / `RegisterScraped` — collectors built from
  `metricdef`, label values bounded (`instruments.go`, `scrape.go`).
- `obsgen/` — dashboards (Grafana Foundation SDK v0.0.20) + metric reference
  page generated from `metricdef`; `CheckDashboard` contract check.
- `InitOTel(ctx, serviceName, log)` — OpenTelemetry trace + meter
  provider wiring (ADR-0927).
- `WaitForShutdown` / `NewShutdownContext` — signal + graceful-drain.

Operator guide: [`docs/development/observability.md`](../../docs/development/observability.md).

## Rebase-sensitive invariants

1. **`InitOTel` returns a no-op shutdown when `OTEL_EXPORTER_OTLP_ENDPOINT`
   is unset.** Documented contract (ADR-0927); existing deployments
   depend on "tracing disabled by default" behaviour.
   `TestInitOTel_NoEndpoint_ReturnsNoop` locks it in. Do not change
   default to "fail closed".

2. **Defaults are ADR-locked.** `DefaultTraceSampleRatio = 0.01` and
   `DefaultMetricExportInterval = 60*time.Second` are guarded by
   `TestDefaults_ADR0927`. Changing them requires updating ADR-0927
   first.

3. **`InitOTel` installs *global* providers via `otel.SetTracerProvider`
   / `otel.SetMeterProvider`.** Library code retrieves them via
   `otel.GetTracerProvider()`, must **never** import SDK packages
   directly. Library calling `sdktrace.NewTracerProvider` directly is
   bug — refactor through `InitOTel`.

4. **OTel is additive to slog + Prometheus.** Phase 1 (ADR-0927)
   explicitly preserves both. Do not delete `NewLogger`, `NewMetrics`,
   or Prometheus `/metrics` endpoint as part of OTel work. slog → OTel
   logs bridge is Phase 3, depends on modernization #3 (zap → slog
   migration) completing first.

5. **One metric definition (HISS-19, WP16 / issue #2430).** New family ->
   `metricdef/families.go` + `All()` first, then registered by every binary
   in its `Emitters` through `NewCounter` / `NewGauge` / `NewHistogram` /
   `RegisterScraped`. Never `prometheus.New*Vec`, GaugeFunc or promauto in a
   service: bypasses label bounds and per-binary contract tests
   (`TestControllerServesEveryFamilyItEmits`,
   `TestNodeServesEveryFamilyItEmits`,
   `TestProvideMetricsServesTheServerFamilies`). Every family registers on
   service's own registry, never `DefaultRegisterer` (ADR-1014). Open label
   limits (`TenantLimit` 256, `ModelLimit` 64, Q-121) -> overflow `other`,
   never drop; raising one moves `seriesBudget` in `metricdef_test.go` and the
   docs.

6. **Generated files never hand-edited.** `deploy/grafana/dashboards/*.json`,
   `docs/observability/metrics.md` come from `go run ./tools/obsgen -write`.
   `obsgen_test.go::TestGeneratedFilesAreCurrent` fails stale copy;
   `TestEveryDashboardQueryIsEmitted` fails panel / annotation / variable
   naming series nothing emits (allow-list `ExternalSeries`, proven by
   `TestExternalSeriesAreServed`). `obsgen/testdata/overview-before-generation.json`
   = frozen negative fixture (5 dead series), never "fixed".
   Bucket matcher: `LeMatcher(bound)` (`le=~"70(\\.0)?"`), never `le="70"`:
   Prometheus 3 stores `le="70.0"`. `CheckBucketMatchers` fails whole-number
   `le` / `quantile` equality; `TestLeMatcherSelectsOneBucket` proves no other
   bucket matched.

7. **Scraped reads never fail /metrics** (`scrape.go`): `RegisterScraped`
   takes `NewReadErrors` counter + `ScrapeGroup{Source, Families, Read}`;
   failed read -> `errs.Inc(source)`, group's families absent this scrape,
   rest served. Never `prometheus.NewInvalidMetric` (whole page 500). New
   source -> value in `metricdef.ReadSource`.

8. **Vendor exporter series only on own dashboard** (`obsgen/exporters.go`,
   `check.go::ExporterSeries`): exporter dashboards tagged
   `vmafx-exporter-<key>`; `CheckDashboard` allows that exporter's series
   only there. Names from exporter's own reference (cite file), never
   guessed. Every generated dashboard passes `dashboard-linter --strict`, no
   exclusions (`scripts/ci/lint-dashboards.sh`, pinned + sha256 in
   `build-config.env`).

9. **Rules generated + tested** (`obsgen/rules.go`, `alerts.go`,
   `rulesfile.go`): `deploy/prometheus/vmafx-rules.yaml` +
   `vmafx-rules.test.yaml` from Go. Each alert name: >=1 firing + >=1
   non-firing promtool case, runbook page
   `docs/observability/runbooks/<slug>.md` (`TestEveryAlertHasARunbook`).
   Annotations use only `{{ $labels.x }}` (rendered in Go for
   `exp_annotations`; promtool compares exactly). Every rule expr
   passes `CheckRules`. CI: `scripts/ci/check-prometheus-rules.sh` (pinned
   promtool, `scripts/ci/pinned-tool.sh` shared with dashboard-linter; one
   fetcher, HISS-19).

10. **Rule settings = chart values** (ADR-2399; `obsgen/settings.go`,
    `helm.go`): objectives, burn windows / factors, alert thresholds live in
    `Settings`. One rule builder, two renderings via `params`:
    `plainParams` (rule file, `-render-rules`), `helmParams`
    (`$.Values.monitoring`, chart template). Burn threshold = PromQL
    `(factor * (1 - objective))`, never number computed in Go or Helm
    (Sprig decimal vs Go float drift). Floats print `fmt.Sprint` (Helm
    float64 print), ints `strconv.Itoa` vs `| int`. Template reads `$.`
    (inside window range). `Settings.Validate` + `ApplyValues` tag check
    refuse what `values.schema.json` refuses
    (`TestValidateAgreesWithTheChartSchema`); new setting -> schema entry
    in same change.

11. **Compose example + smoke** (`deploy/compose/observability`,
    `tools/obssmoke`, `obsgen/datasources.go`): data source provisioning
    generated, URLs = compose service names, uids = `PrometheusUID` /
    `TempoUID` / `LokiUID`. Smoke parses dashboards only via
    `obsgen.DashboardQueries` (one parser with `CheckDashboard`). Exemption =
    query without data on CPU stack + reason; exempted query with data
    fails run. `tools/obsgen` writes 0644 / dirs 0755 (containers read
    mounts as other users). Smoke never in parallel with another heavy job;
    stack torn down after (`down -v`).

12. **Settings series** (`obsgen/settingrules.go`, Q-193 / Q-195): SLO
    objectives, SLO event rates, prices = recording rules from `Settings`,
    one sample per scraped instance (`perScrapedInstance`, `job` +
    `instance` kept: dashboard-linter needs both matchers). Price rule
    `... > 0`: unset price records nothing, cost panels empty; never assume
    price. One price per job-second (every outcome) + one per completed or
    failed job (`chargedOutcomes`, Q-208; cancelled never charged, promtool
    case in rules test file), every backend. Smoke sets test prices in `XTS`
    only.

## Test requirements

```bash
go test ./pkg/observability/...
```

Tests must not require live OTel collector. Use bogus endpoint
(`http://127.0.0.1:14318`) — OTLP HTTP exporter constructed lazily,
`shutdown` bounded by test context.

Additional invariants locked in by `coverage_gaps_test.go`:

- `WaitForShutdown` must return after receiving SIGTERM (not only on
  context cancel). `TestWaitForShutdown_SIGTERMDelivery` sends real
  SIGTERM to `os.Getpid()`; do not run that test with `t.Parallel()`.
- `AttrJobID`, `AttrModel`, `AttrBackend`, `AttrNodeID`, `AttrVendor`,
  `AttrStatus` key strings are schema-locked to values in ADR-0782.
  Any rename is breaking OTLP consumer change — update ADR first.

## OTel rollout discipline

Rollout is complete (epic #1241, ADR-0782): every Go binary initialises
OpenTelemetry through `internal/app/bootstrap` — `bootstrap.Base`
carries golusoris's `otel.Module` (ADR-1119) plus `service.name` /
`service.version` decorator — and nowhere else. See `cmd/AGENTS.md` #1.

`InitOTel` in this package is ADR-0927 Phase 1 helper that predates
golusoris migration. **No `cmd/` binary calls it any more** (ADR-1119
replaced per-binary "InitOTel + shutdown dance" with `bootstrap.Base`);
remains because ADR-0927 is Accepted, contract (invariants 1–3 above)
still tested. Do not wire it into binary next to `bootstrap.Base` —
two global providers cannot coexist (golusoris `otel/AGENTS.md`:
"Don't wire two OTel modules"). Retiring it is superseding-ADR
decision, not cleanup.

What new service or new request path does instead:

- fx service: start from `bootstrap.Base`; add `bootstrap.HTTPTracing`
  next to `bootstrap.HTTP`; gRPC spans come with `grpc.Module`.
- one-shot CLI: build `bootstrap.Base` per invocation
  (`cmd/vmafx-tune/cmd/golusoris.go::withGolusoris` is template).
- application spans: `observability.StartSpan` / `EndSpan` with name
  from `otel_instruments.go` (`SpanJobSubmit`, `SpanScoring`,
  `SpanFrameExtraction`, `SpanONNXInference`, `SpanMCPTool`,
  `SpanTuneCommand`) — add constant here first; never inline string.
- tests: `internal/oteltest.Recorder` installs in-memory recorder as
  global provider (no collector needed); such tests must not be
  parallel.

## `ObserveScoreLatency` context requirement (ADR-1095)

`ObserveScoreLatency(ctx, om, start, attrs...)` takes caller's request
context as first argument. Pass handler `ctx`, not
`context.Background()` — SDK reads active span ID from context to
attach trace exemplars to histogram data points. Using
`context.Background()` silently discards baggage, breaks exemplar
linking.
