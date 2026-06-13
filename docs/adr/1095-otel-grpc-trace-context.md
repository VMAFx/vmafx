# ADR-1095: Fix OTel trace context propagation across gRPC boundaries

| Field       | Value                                          |
|-------------|------------------------------------------------|
| **Status**  | Accepted                                       |
| **Date**    | 2026-06-07                                     |
| **Deciders**| Lusoris                                        |
| **Tags**    | observability, otel, grpc, bug                 |

## Context

ADR-0927 introduced `pkg/observability.InitOTel`, which installs the global
OTel `TracerProvider`, `MeterProvider`, and a W3C `TextMapPropagator`
(`TraceContext` + `Baggage`) on startup for all four Go binaries.

ADR-0782 defined the span schema and instrumented the `vmafx-controller`
gRPC server with `otelgrpc.NewServerHandler()` so incoming RPCs create
server spans parented to the caller's trace.

Two gaps were left unaddressed:

1. **`vmafx-server` gRPC server** (`cmd/vmafx-server/grpc_server.go`
   `runGRPCWithServer`) — the `otelgrpc.NewServerHandler()` stats handler
   was never installed.  Any `traceparent` header arriving from a client
   was silently discarded; every server span appeared as an unrooted trace
   root rather than a child span of the calling process.

2. **`pkg/score` gRPC client** (`pkg/score/grpc_client.go` `Dial`) — the
   `otelgrpc.NewClientHandler()` stats handler was never attached to the
   `grpc.NewClient` call.  Outgoing RPCs carried no `traceparent` header,
   so the controller-side `otelgrpc.NewServerHandler()` received no parent
   span ID and could not link the two sides of the call into one trace.

3. **`ObserveScoreLatency` context** (`pkg/observability/otel_instruments.go`)
   — the helper passed `context.Background()` to the SDK histogram `Record`
   call, discarding baggage and preventing exemplar attachment (OTel SDK
   uses the context to read the active span ID for attaching trace exemplars
   to histogram data points).

The net effect: every scoring trace appeared as a forest of disconnected
roots.  The controller's `vmafx.job.submit` span, the server's (missing)
span, and the node's `vmafx.scoring` / `vmafx.frame.extraction` spans were
all unlinked, making distributed tracing unusable for debugging end-to-end
latency.

## Decision

Fix all three gaps in a single PR:

1. Add `grpc.StatsHandler(otelgrpc.NewServerHandler())` to the `grpc.NewServer`
   call inside `runGRPCWithServer` in `cmd/vmafx-server/grpc_server.go`.
   This mirrors the pattern already present in `cmd/vmafx-controller/grpc_server.go`.

2. Add `grpc.WithStatsHandler(otelgrpc.NewClientHandler())` to the
   `grpc.NewClient` call inside `Dial` in `pkg/score/grpc_client.go`.

3. Change `ObserveScoreLatency` to accept a `context.Context` as its first
   argument and forward it to `ScoreLatency.Record`.  All call sites are in
   test code only (no production callers existed before this PR).

No new ADR is required for options 1 and 2 — they are straightforward
application of the pattern already documented in ADR-0927.  An ADR is filed
here because the combined gap represents an architectural correctness defect
(distributed tracing silently broken across all scoring paths) that another
engineer could revisit.

## Consequences

- Distributed traces that cross the HTTP/gRPC boundary between a score client
  and `vmafx-server` are now correctly linked.
- The controller, server, and node spans for a single scoring job appear in
  the same trace waterfall in Jaeger / Grafana Tempo.
- `ObserveScoreLatency` is a breaking API change to the function signature;
  the function is package-internal (no callers outside `pkg/observability`
  and its tests), so no downstream impact.

## Alternatives considered

| Alternative | Reason rejected |
| --- | --- |
| Apply only the client-side fix (option 2) and leave the server without the stats handler | The server still creates no span, so Jaeger shows the trace ending at the client and the controller's span appearing disconnected. Both sides must be instrumented. |
| Add a middleware interceptor instead of the stats handler | `otelgrpc` recommends the `StatsHandler` API over the deprecated `UnaryInterceptor` wrappers. The interceptor approach works but produces lower-quality metadata (missing internal event attributes that the stats handler captures). |
| Keep `ObserveScoreLatency` with `context.Background()` | The only downside is missing exemplar linking — a minor quality issue, not a correctness defect. However the fix is trivial (one arg) and adds semantic correctness at zero cost. |

## References

- ADR-0927: OpenTelemetry traces + metrics Phase 1 (provider initialisation).
- ADR-0782: OpenTelemetry tracing and metrics schema (span names, attributes).
- `go.opentelemetry.io/contrib/instrumentation/google.golang.org/grpc/otelgrpc`
  v0.53.0 (already in `go.mod`).
