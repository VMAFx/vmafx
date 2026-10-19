# Research-2399: Packaging the generated rules and dashboards in the Helm chart

- **Status**: Active
- **Workstream**: ADR-2399, ADR-2349
- **Last updated**: 2026-10-08

## Question

How do the generated rules and dashboards ship in the Helm chart so that the
SLO objectives and alert thresholds are chart values, the chart and a plain
rule file rendered from the same values agree, and every component is
actually scraped once?

## Sources

- Prometheus 3 migration guide, "le and quantile label values"
  (<https://prometheus.io/docs/prometheus/latest/migration/>).
- Prometheus 3.15.0 (the release pinned in `build-config.env`), run locally
  against a text-format exposition.
- Helm v4.1.3 (local) rendering a scratch chart; Sprig function reference for
  `mulf`, `subf`, `round`, `uniq`.
- prometheus-operator CRD schemas from the CRDs catalog
  (<https://github.com/datreeio/CRDs-catalog>), checked with
  `kubeconform -strict`.
- `go.yaml.in/yaml/v3` decoding behaviour, observed in
  `TestValidateAgreesWithTheChartSchema`.

## Findings

1. **Prometheus 3 stores a whole-number `le` in float form.** The migration
   guide says a classic histogram's `le` (and a summary's `quantile`) is
   normalised on ingestion, so a rule or dashboard matching `le="1"` stops
   working. Checked on Prometheus 3.15.0: a text-format exposition of
   `h_bucket{le="30"}` is stored as `le="30.0"`; `h_bucket{le="30"}` returns
   nothing and `h_bucket{le=~"30(\\.0)?"}` returns the bucket. The latency
   SLO (`le="30"`) and the Quality dashboard's share below 70 (`le="70"`)
   matched nothing; `obsgen.LeMatcher` now writes the regex form and the
   checks refuse the equality form. The promtool test inputs carry the
   stored form, and the old latency rule fails them.
2. **Helm prints a float64 value with `fmt.Sprint`.** Helm decodes every
   number of a values file to `float64`; `{{ .Values.x }}` prints `14.4`,
   `6`, `1800`, but `1e+06` for 1000000 and `1.23456789e+08` for 123456789,
   exactly as `fmt.Sprint` does. `| int` prints integers in full. The plain
   renderer therefore prints floats with `fmt.Sprint` and the integer
   settings with `strconv.Itoa` against `| int` in the template.
3. **Computing thresholds in Helm drifts.** Sprig's `mulf` and `subf` use
   decimal arithmetic and `round` rounds to decimal places, while Go's
   `%.6g` rounds to significant digits: for a factor of 13.37 and an
   objective of 0.99999 the two give 0.000134 and 0.0001337. Writing the
   threshold as the PromQL expression `(factor * (1 - objective))` leaves the
   arithmetic to Prometheus and keeps the two renderings identical.
4. **`--set` passes a decimal as a string.** `--set
   monitoring.slo.jobSuccess=0.995` fails the schema with "got string, want
   number"; `--set-json` and values files pass a number.
5. **Inside `range`, `.` is the element.** The recording rules range over the
   windows, so the template reads every value through `$.Values`.
6. **The decoder converts silently.** `yaml.v3` decodes `1.5` into an `int`
   field and the number `30` into a `string` field without an error, where the
   chart's schema refuses both. `Settings.ApplyValues` checks each scalar's
   YAML tag before decoding; the test that holds the schema and `Validate` to
   the same cases found it.
7. **Pre-existing chart defects.** With `workload: StatefulSet` the server's
   ServiceMonitor matched the headless Service too, so every pod was scraped
   twice and `sum()` doubled; a ServiceMonitor placed in another namespace
   had no `namespaceSelector` and selected nothing; the node's HTTP listener
   (the `/metrics` port) was not pinned to `node.metricsPort`.
8. **CRD shapes.** The rendered ServiceMonitors, PodMonitor and
   PrometheusRule pass `kubeconform -strict` against the operator's CRD
   schemas.

9. **Compose example (2026-10-08).** Checked against each component's own
   release: the OpenTelemetry Collector 0.162 names its exporters
   `otlp_grpc` and `otlp_http` (`otelcol validate` refuses an unknown type);
   Loki 3.7 takes OTLP logs at `http://loki:3100/otlp`; Tempo 3.1's
   single-binary example needs only `distributor.receivers` and local
   storage; Prometheus 3.15 still enables exemplars with
   `--enable-feature=exemplar-storage`. Grafana (uid 472) could not read
   provisioning files `obsgen -write` had created with mode `0600`; it now
   writes `0644`.
10. **No service exported anything.** The smoke test found no trace in Tempo
    and no span at the collector: fx builds golusoris's `*otel.Providers`
    only for a dependant, and no service had one (#2545). With that fixed,
    `VMAFX_OTEL_ENDPOINT=otel-collector:4317` and
    `OTEL_EXPORTER_OTLP_ENDPOINT=http://otel-collector:4317` deliver traces;
    the documented `OTEL_EXPORTER_OTLP_ENDPOINT=otel-collector:4317` sends to
    `localhost:4317`, because the SDK reads that variable as a URL.
11. **Panels need settled scrapes.** Queried right after the traffic, the
    rate panels returned nothing (one sample in the window); after six scrapes
    (30 s at 5 s) every query but the GPU memory panel returns data. A
    counter child created at zero (device-memory read errors, requeues)
    returns data too.

12. **Settings in dashboards (2026-10-08).** dashboard-linter v0.3.0 requires
    `job=~"$job"` and `instance=~"$instance"` on every selector
    (`rule_target_job_instance.go`), and a recorded series loses both labels
    when its expression aggregates them away. The settings series are
    therefore recorded per scraped instance
    (`max by (job, instance) (vmafx_build_info) * 0 + value`). The linter's
    `$__rate_interval` rule applies to `rate` and `irate` only, so the reports
    use `increase` and `sum_over_time` over `$__range`.
13. **Counters that start with a value.** Prometheus does not count a
    counter's first sample: a burst of submits before the series' first
    scrape reads as an increase of 0. A panel that needs growth to have data
    (time until demand meets capacity, `deriv(...) > 0`) is empty or not by
    the scrape timing; the capacity dashboard shows the demand's growth
    instead, which always has data once two points exist.

## Alternatives explored

- A hand-written Helm template beside the generated rule file: two copies of
  every rule with nothing comparing them.
- Thresholds computed by Helm: finding 3.
- Rendering the Compose rule file with `helm template` and extracting the
  PrometheusRule's `spec`: needs Helm and an extraction step in the Compose
  stack, where `go run ./tools/obsgen -render-rules` writes the file from the
  same rule code.
- Driving the Compose smoke from the host on published ports: the host's own
  services collide with fixed ports. The smoke runs as a Compose service in
  the example's network, and the published ports are ephemeral in the run.

## Open questions

- A short window longer than its long window is accepted by both the schema
  and `Validate` (JSON Schema cannot compare two durations); the rules still
  render and evaluate.

## Related

- [ADR-2399](../adr/2399-observability-slo-settings-as-values.md),
  [ADR-2349](../adr/2349-observability-package.md), issue #2430.
- [Monitoring on Kubernetes](../observability/kubernetes.md).
