<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
<!-- markdownlint-disable MD013 -->
# AGENTS.md — cmd/

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../docs/development/agents-index.md)).

7 Go binaries. Per-binary guides next to each `main.go`
(`cmd/vmafx-*/AGENTS.md`). Invariants spanning all binaries below.

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `vmafx-*/**/*.go` | [binary-overview](AGENTS.d/binary-overview.md) | Every root sets EnvPrefix VMAFX_; span tests use internal/oteltest without t.Parallel. |
| `vmafx-ort-runner/*.go`, `/pkg/ai/*.go` | [ort-runner-no-otel](AGENTS.d/ort-runner-no-otel.md) | vmafx-ort-runner stays OTel-free; inference span owned by caller pkg/ai.Registry.Infer. |
| `vmafx-*/**/*.go`, `/internal/app/bootstrap/*.go`, `/pkg/observability/otel_instruments.go`, `/pkg/score/*.go` | [otel-and-tracing](AGENTS.d/otel-and-tracing.md) | Init OTel via bootstrap.Base only; HTTP/gRPC spans from bootstrap + golusoris; ADR-0782 names. |
| `/internal/app/scoringservice/*.go`, `vmafx-server/*.go`, `vmafx-controller/*.go` | [scoring-service-owner](AGENTS.d/scoring-service-owner.md) | Server and controller share scoring-service plumbing in internal/app/scoringservice. |
