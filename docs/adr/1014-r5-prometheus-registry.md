<!-- markdownlint-disable MD013 -->
# ADR-1014: Prometheus registry isolation for SetControllerSources

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `security`, `observability`, `go`

## Context

`SetControllerSources` in `pkg/observability/observability.go` registered the
three live-gauge GaugeFunc metrics (`jobs_pending`, `jobs_running`,
`nodes_live`) against `prometheus.DefaultRegisterer` — the process-global
Prometheus registry — even though `NewMetrics` accepts an isolated
`*prometheus.Registry` to avoid global-state pollution.

Two bugs arose from this discrepancy (r5-prometheus-cardinality scan findings):

1. The three gauges were **invisible on the `/metrics` endpoint**, which serves
   `promhttp.HandlerFor(registry, ...)` from the isolated registry only.
2. Any second invocation of `SetControllerSources` (test re-run, supervisor
   restart, or future hot-reload path) panicked with
   `prometheus.AlreadyRegistered` from the global default registerer. The test
   at `observability_test.go:177` acknowledged this with a `defer/recover()`
   workaround, confirming the bug was known but unfixed.

## Decision

We will store the `prometheus.Registerer` that `NewMetrics` receives as a
private field `reg` on `Metrics`, and use `promauto.With(m.reg)` inside
`SetControllerSources` to register the GaugeFuncs against the same isolated
registry. We will also guard `SetControllerSources` with a `sync.Once` so a
second call is a no-op rather than a panic.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep global DefaultRegisterer, add `once` guard | Simpler one-line fix | Gauges still invisible on isolated endpoint; test environment still affects production binary state | Does not fix the visibility bug |
| Accept a `prometheus.Registerer` arg in `SetControllerSources` | Avoids field storage | Breaks existing call sites; two registries could still diverge | More invasive API change than necessary |
| Use `prometheus.DefaultRegisterer` for everything (revert isolation) | No dual-registry confusion | Defeats test isolation; cannot run multiple controllers in-process | Regresses ADR-0703 design intent |

## Consequences

- **Positive**: The three controller gauges are now visible on `/metrics`.
  Tests no longer need `defer/recover()` around `SetControllerSources`.
  Second calls are safe.
- **Negative**: `Metrics` struct gains a private `reg` field and a
  `sourcesOnce sync.Once` field; this is a minor allocation overhead
  (two pointer-sized words per `Metrics`).
- **Neutral / follow-ups**: The existing test
  `TestSetControllerSources_RegistersGauges` was updated to assert against
  `reg.Gather()` rather than `prometheus.DefaultGatherer`. New test
  `TestSetControllerSources_Idempotent` added.

## References

- r5-prometheus-cardinality scan findings (observability.go:152-173)
- `observability_test.go:177-183` — the `defer/recover` workaround that
  confirmed the bug
