---
paths:
  - cmd/vmafx-controller/metrics*.go
  - cmd/vmafx-controller/main.go
  - cmd/vmafx-controller/app_test.go
invariant: Metrics from metricdef + observability helpers only; OTel via bootstrap.Base + HTTPTracing.
---
<!-- markdownlint-disable MD013 MD029 MD060 -->
# Controller metrics and OpenTelemetry spans

## Invariants

### observability

1. **Metrics from `metricdef` only** (`metrics.go`, WP16 / issue #2430):
   controller families = `metricdef.ByEmitter(metricdef.Controller)`. Event
   families via `observability.NewCounter/NewHistogram`
   (`newControllerMetrics`); queue + registry families read at scrape time
   via `observability.RegisterScraped` (`registerQueueCollector`, one
   `Stats` query per scrape, `observability.ScrapeTimeout`). No
   `prometheus.New*Vec` / GaugeFunc / promauto here. Guard:
   `metrics_test.go::TestControllerServesEveryFamilyItEmits`.

2. **OTel from `bootstrap.Base` + `bootstrap.HTTPTracing`, spans from golusoris and `grpc_server.go`**
   (`main.go::productionOptions`, ADR-0782 / ADR-1119): `bootstrap.HTTPTracing`
   traces `POST /v1/score` with `otelhttp`; gRPC server spans via `grpcmod.Module`
   `otelgrpc`; child span `vmafx.job.submit` via `observability.StartSpan`.
   Guards: `app_test.go::TestOTelWiredThroughBootstrap` locks no-op default,
   `vmafx-controller` / `pkg/version`; `TestGRPCHealthEmitsLinkedSpans` proves
   propagation (client/server share trace).
