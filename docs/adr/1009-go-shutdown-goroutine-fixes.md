<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1009: Fix Go shutdown / goroutine correctness — WaitForShutdown unconditional block, unbounded GracefulStop

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `go`, `server`, `controller`, `shutdown`, `correctness`

## Context

Two related signal-handling / shutdown correctness defects identified by the r3/r4 audit:

1. **`pkg/observability/observability.go:198`** — `WaitForShutdown` ends with an
   unconditional `<-time.After(timeout)` (the "drain window").  When the signal
   arrives and all in-flight requests complete in milliseconds, the function still
   stalls for the full `GracefulShutdownTimeout` (30 s) before returning, making
   every clean shutdown artificially slow.  The `ctx.Done()` arm is only checked in
   the _wait-for-signal_ select above; the drain window had no such arm.

2. **`cmd/vmafx-server/grpc_server.go:188` and `cmd/vmafx-controller/grpc_server.go:434`**
   — `srv.GracefulStop()` is called with no timeout inside the shutdown goroutine.
   A stuck streaming RPC (e.g., a client that never sends EOF) will block
   `GracefulStop` indefinitely.  Because `srv.Serve` waits for all connections to
   close before returning, the server process never exits after SIGTERM, blocking
   `defer scorer.Close()`, `defer jobQueue.Close()`, and any pod-termination hook.

Note: `NewShutdownContext` was already fixed in ADR-0978 to use
`signal.NotifyContext` (no goroutine leak).  The `reaper` goroutine in
`cmd/vmafx-controller/nodes/registry.go` was fixed in ADR-0962.  This ADR
addresses the remaining two issues from the audit.

## Decision

1. **`WaitForShutdown`**: replace the bare `<-deadline` with
   `select { case <-time.After(timeout): case <-ctx.Done(): }` so the function
   returns as soon as the context is done (servers stopped) without waiting for the
   full timeout.

2. **`runGRPCWithServer` / `RunGRPC`**: wrap `GracefulStop()` in a goroutine and
   race it against `time.After(GracefulShutdownTimeout)`.  On timeout, call
   `srv.Stop()` (hard stop) and log a warning.  This bounds the worst-case shutdown
   time to `2 × GracefulShutdownTimeout` (once for the drain window in
   `WaitForShutdown`, once for the gRPC hard-stop fallback).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep unconditional drain window | Simple | 30 s stall on every clean shutdown | Rejected |
| Remove drain window entirely | Instant exit | In-flight requests get no drain time | Rejected |
| Use `grpc.Server.GetServiceInfo` to detect stuck streams | Precise | Complex introspection | Overkill for this use case |
| Hard-stop fallback at `GracefulShutdownTimeout` | Bounds worst-case | Forcibly aborts stuck RPCs | Chosen — correct trade-off |

## Consequences

- **Positive**: Clean shutdowns no longer stall for 30 s; stuck streaming RPCs
  cannot prevent process exit after SIGTERM.
- **Neutral**: The drain window is still respected (`ctx.Done()` arm only fires
  after the caller cancels the context, which happens after servers stop).
- **Negative**: A truly hung streaming RPC will be hard-stopped after 30 s instead
  of waiting forever — this is the desired behaviour.

## References

- r3/r4 audit findings: `[r3-signal]` cluster
- ADR-0978 (NewShutdownContext goroutine-leak fix)
- ADR-0962 (reaper goroutine ctx propagation)
