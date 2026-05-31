<!-- markdownlint-disable MD013 MD036 MD060 -->
# ADR-0962: Controller fixes — implement StreamJobs snapshot and add reaper stop signal (round-25 audit B.3 + B.4)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: controller, grpc, go, correctness, goroutine, phase4b, fork-local

## Context

A round-25 audit of `cmd/vmafx-controller` identified two correctness bugs:

**B.3 — `controllerServer.StreamJobs` is a no-op**

`StreamJobs` (file: `grpc_server.go:180–185`) logs a single line and returns
`nil`.  The server-streaming RPC therefore sends zero messages to every caller.
A client that invokes `StreamJobs` to inspect queue state will receive an
immediately-closing stream and silently infer "no jobs queued" — the same
false-success pattern noted for MCP tools in the project memory (analogous to
`isError=False` on an error path).

**B.4 — `nodes.Registry.reaper` goroutine has no stop signal**

`NewRegistry` spawned `go r.reaper()` with no cancellation path.  The
`reaper(_ ...context.Context)` signature accepted a variadic context but the
call site passed nothing, so the goroutine looped on `ticker.C` forever.  In
production this is benign (goroutine lifetime == process lifetime), but in
tests each `NewRegistry` call leaked one goroutine.

## Decision

**B.3** — implement the snapshot: call `queue.ListAll(ctx, statusFilter)` to
obtain a point-in-time list of jobs matching the optional proto status filter,
then iterate and call `stream.Send(queueJobToProto(j))` for each job.  Add
`ListAll(ctx, []string) ([]*Job, error)` to the `Queue` interface and
`SQLiteQueue` (parameterised `SELECT … WHERE status IN (…)` with a fallback
unconditional query when no filter is supplied).  Add `protoStatusToQueue`
helper (inverse of existing `queueStatusToProto`) to translate the proto enum
filter into queue status strings.

**B.4** — change `NewRegistry` signature to `NewRegistry(ctx context.Context,
log *slog.Logger) *Registry`.  Store a derived cancellable context internally;
add a `Close()` method that cancels it.  Change `reaper(_ ...context.Context)`
to `reaper(ctx context.Context)` and replace `for range ticker.C` with a
`select { case <-ctx.Done(): return; case <-ticker.C: ... }` loop.  Propagate
from `main.go` (shutdown context arrives from `observability.NewShutdownContext`
which fires on SIGTERM/SIGINT).  All call sites in tests updated to pass
`context.Background()` + `t.Cleanup(func() { r.Close() })`.

## Alternatives considered

| Option | Pros | Cons |
|--------|------|------|
| **B.3: Return `codes.Unimplemented`** | Honest, caller gets a clear gRPC error | Feature remains unusable; breaks any client that expected the docstring contract |
| **B.3: Implement snapshot (chosen)** | Fulfils the documented Phase 4b.1 promise; unblocks CLI/MCP usage | Requires adding `ListAll` to the `Queue` interface — minor surface growth |
| **B.4: Internal `Stop()` channel** | No public API change | Requires storing a separate `chan struct{}`; `context.Context` is the idiomatic Go pattern |
| **B.4: `NewRegistry(ctx, log)` (chosen)** | Idiomatic; propagates lifetime from caller; `Close()` is an idempotent double-cancel no-op | Breaks call-site signature — one-time mechanical update |

## Consequences

**Positive**

- `StreamJobs` now delivers actual queue state; CLI and MCP clients that call
  it will receive correct data instead of false-empty responses.
- Reaper goroutines stop cleanly on SIGTERM / test teardown; no goroutine leaks
  in `go test -count=N` runs.
- `ListAll` exposes a generally useful snapshot capability that future HTTP
  endpoints (`GET /v1/jobs`) can reuse.

**Negative**

- `Queue` interface grows by one method (`ListAll`).  Any in-tree mock
  implementations must be updated — currently none exist; all tests use the
  real `SQLiteQueue`.

**Neutral**

- `NewRegistry` signature change is a compile-time break; all call sites in
  tree were updated in the same PR.

## References

- Bug report: round-25 infrastructure audit B.3 + B.4 (2026-05-31 session)
- Memory: `project_mcp_iserror_must_be_true` — analogous silent-success
  anti-pattern (B.3)
- ADR-0711: vmafx-controller Phase 4b.1 scope expansion (origin)
- ADR-0709: VMAFX Phase 4b distributed platform (umbrella)
