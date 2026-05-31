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
   single typed sum without coordinated updates to every caller.
