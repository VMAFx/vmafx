# AGENTS.md — pkg/score

Go client wrapper around `vmafxv1.VmafxScoringClient`. See
[ADR-0703](../../docs/adr/0703-vmafx-server-go-grpc.md) for the unary
surface and [ADR-0933](../../docs/adr/0933-grpc-streaming-multi-frame-scoring.md)
for the streaming surface.

## Rebase-sensitive invariants

1. **`Client.Score` signature stability** (`grpc_client.go`): the
   `(reference, distorted, model string) -> (float64, map[string]float64, error)`
   signature mirrors the unary v1 RPC. Existing callers (vmafx-tune,
   vmafx-controller, MCP) depend on this shape; never remove or rename
   the positional arguments. Add new options via a functional-options
   struct, not by extending the positional list.

2. **`ScoreStream.PushFrame` ordering** (`grpc_client.go`): `frameIndex`
   must be strictly monotonically increasing from 0 to match the server's
   contract (ADR-0933). The wrapper does not validate this client-side
   because the server is the source of truth — adding redundant client
   validation invites drift if the server contract is ever relaxed.

3. **Recv terminal-aggregate semantics** (`grpc_client.go`): `Recv`
   returns `(*FrameScore, *Aggregate, error)` where exactly one of the
   first two is non-nil per call, and `io.EOF` signals the stream is
   fully drained after the terminal `Aggregate` was already returned.
   Callers loop until `io.EOF`; do not change the return shape to a
   single typed sum without coordinated updates to every caller. Recv
   compares the EOF sentinel via `errors.Is(err, io.EOF)` (ADR-0978);
   do not regress to `err == io.EOF` — the defensive form keeps the
   EOF semantics stable if a future gRPC release wraps the sentinel
   inside a status.

4. **OTel client handler on `Dial`** (`grpc_client.go`, ADR-1095): `grpc.NewClient`
   must always include `grpc.WithStatsHandler(otelgrpc.NewClientHandler())`. Without
   this the outgoing RPCs carry no `traceparent` header and the controller/server
   spans cannot be linked into the same distributed trace. The handler is a no-op
   when `InitOTel` installed no-op providers (i.e. `OTEL_EXPORTER_OTLP_ENDPOINT`
   is unset), so it is safe to keep unconditionally.

5. **Send-EOF translation** (`grpc_client.go`, ADR-0978): both
   `OpenScoreStream` and `PushFrame` route Send errors through
   `recvStatusOnEOF`, which detects an `io.EOF` from Send (gRPC's
   "stream already done from the server's perspective" signal) and
   calls Recv to retrieve the server's actual non-OK status. The
   caller-visible error is then the real `codes.Foo` from the server
   (e.g. `InvalidArgument: ScoreStream: first message must set the
   config oneof`) rather than the meaningless bare `EOF`. Removing
   this translation would re-introduce the regression where every
   server-side rejection of a malformed StreamConfig surfaces as
   "send StreamConfig: EOF" with no clue about what was actually
   wrong.
