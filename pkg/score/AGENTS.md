# AGENTS.md — pkg/score

Go client wrapper around `vmafxv1.VmafxScoringClient`. See
[ADR-0703](../../docs/adr/0703-vmafx-server-go-grpc.md) for unary
surface, [ADR-0933](../../docs/adr/0933-grpc-streaming-multi-frame-scoring.md)
for streaming surface.

## Rebase-sensitive invariants

1. **`Client.Score` signature stability** (`grpc_client.go`): the
   `(reference, distorted, model string) -> (float64, map[string]float64, error)`
   signature mirrors unary v1 RPC. Existing callers (vmafx-tune,
   vmafx-controller, MCP) depend on this shape; never remove or rename
   positional arguments. Add new options via functional-options struct,
   not by extending positional list.

2. **`ScoreStream.PushFrame` ordering** (`grpc_client.go`): `frameIndex`
   must be strictly monotonically increasing from 0, match server's
   contract (ADR-0933). Wrapper does not validate this client-side:
   server is source of truth. Adding redundant client validation
   invites drift if server contract ever relaxed.

3. **Recv terminal-aggregate semantics** (`grpc_client.go`): `Recv`
   returns `(*FrameScore, *Aggregate, error)`; exactly one of first two
   non-nil per call. `io.EOF` signals stream fully drained, after
   terminal `Aggregate` already returned. Callers loop until `io.EOF`;
   do not change return shape to single typed sum without coordinated
   updates to every caller. Recv compares EOF sentinel via
   `errors.Is(err, io.EOF)` (ADR-0978); do not regress to
   `err == io.EOF` — defensive form keeps EOF semantics stable if
   future gRPC release wraps sentinel inside status.

4. **OTel client handler on `Dial`** (`grpc_client.go`, ADR-1095):
   `grpc.NewClient` must always include
   `grpc.WithStatsHandler(otelgrpc.NewClientHandler())`. Without this,
   outgoing RPCs carry no `traceparent` header; controller/server spans
   cannot link into same distributed trace. Handler is no-op when
   `InitOTel` installed no-op providers (i.e.
   `OTEL_EXPORTER_OTLP_ENDPOINT` unset) — safe to keep unconditionally.

5. **Send-EOF translation** (`grpc_client.go`, ADR-0978): both
   `OpenScoreStream` and `PushFrame` route Send errors through
   `recvStatusOnEOF`: detects `io.EOF` from Send (gRPC's "stream
   already done from server's perspective" signal), calls Recv to
   retrieve server's actual non-OK status. Caller-visible error is
   then real `codes.Foo` from server (e.g. `InvalidArgument:
   ScoreStream: first message must set the
   config oneof`) rather than the meaningless bare `EOF`. Removing
   this translation re-introduces regression: every server-side
   rejection of malformed StreamConfig surfaces as "send StreamConfig:
   EOF" with no clue what was wrong.
