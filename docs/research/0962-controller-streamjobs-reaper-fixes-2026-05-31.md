<!-- markdownlint-disable MD013 MD060 -->
# Research digest — Controller infrastructure fixes (ADR-0962)

**Date**: 2026-05-31
**Scope**: round-25 audit findings B.3 (`StreamJobs` no-op) and B.4 (`reaper` goroutine leak)
**PR**: fix/grpc-streamjobs-and-reaper-stop

---

## Finding B.3 — `StreamJobs` silent empty response

### Root cause

`controllerServer.StreamJobs` in `cmd/vmafx-controller/grpc_server.go` had a
body consisting of a single log line followed by `return nil`.  A
server-streaming gRPC handler that returns `nil` closes the stream
successfully — from the client's perspective this is indistinguishable from
"zero matching jobs exist."

This is the Go analogue of the `isError=False` silent-success anti-pattern
(project memory: `project_mcp_iserror_must_be_true`): the call appears to
succeed while conveying no information.

### Queue interface gap

The `Queue` interface had no method to enumerate all jobs.  Individual jobs
could be fetched by ID (`Get`), but there was no safe snapshot iterator.

**Resolution**: add `ListAll(ctx context.Context, statuses []string) ([]*Job, error)` to both the interface and the `SQLiteQueue` concrete type.  The
implementation issues a single parameterised `SELECT … WHERE status IN (…)`
query (or an unconditional `SELECT` when no filter is specified), scans into
`Job` structs, and returns copies to avoid caller-side data races.

A `repeatCommaQ(n int) string` helper produces the `?,?,…` placeholder
expansion needed for the variadic `IN` clause.

### Filter translation

The proto `StreamJobsRequest.status_filter` carries `Job.Status` enum values.
A new `protoStatusToQueue(controllerv1.JobStatus) string` helper (inverse of
the existing `queueStatusToProto`) translates each enum value into the queue's
canonical status string before passing the slice to `ListAll`.

---

## Finding B.4 — `reaper` goroutine never stops

### Root cause

```go
// Old signature — variadic context accepted but never passed by caller.
func (r *Registry) reaper(_ ...context.Context) {
    ticker := time.NewTicker(HeartbeatTimeout / 3)
    defer ticker.Stop()
    for range ticker.C { /* ... */ }
}
```

The call site `go r.reaper()` passed no arguments.  The variadic `_
...context.Context` parameter was purely cosmetic — the loop ran until the
process exited.

In production this is benign: the registry lives for the lifetime of the
controller process, so the goroutine naturally terminates when the OS reclaims
memory.  In tests, however, every `NewRegistry` call leaked one goroutine; a
`-count=100` run accumulates 100 background tickers.

### Fix pattern

Idiomatic Go cancellation: store a derived `context.CancelFunc` inside
`Registry`, propagate from the caller's context, replace `for range ticker.C`
with `select { case <-ctx.Done(): return; case <-ticker.C: ... }`.

```go
// New — required non-variadic context.
func (r *Registry) reaper(ctx context.Context) {
    ticker := time.NewTicker(HeartbeatTimeout / 3)
    defer ticker.Stop()
    for {
        select {
        case <-ctx.Done():
            return
        case <-ticker.C:
            // evict stale nodes
        }
    }
}
```

`Close()` cancels the internal context; it is idempotent (`context.WithCancel`
returns a cancel func that is safe to call multiple times).

### Call-site updates

`NewRegistry` signature changed from `NewRegistry(log *slog.Logger)` to
`NewRegistry(ctx context.Context, log *slog.Logger)`.  All three call sites
updated:

| File | Change |
|------|--------|
| `cmd/vmafx-controller/main.go` | Pass shutdown ctx; `defer nodeRegistry.Close()` |
| `cmd/vmafx-controller/nodes/registry_test.go` | Pass `context.Background()`; `t.Cleanup(r.Close)` |
| `cmd/vmafx-controller/scheduler/scheduler_test.go` | Pass `context.Background()`; `t.Cleanup(r.Close)` |

---

## Test coverage added

| Test | Package | Covers |
|------|---------|--------|
| `TestStreamJobs_EmptyQueue_ReturnsOK` | `cmd/vmafx-controller` | B.3: empty queue → zero messages, no error |
| `TestStreamJobs_WithJobs_StreamsSnapshot` | `cmd/vmafx-controller` | B.3: 3 jobs → 3 messages, correct IDs |
| `TestStreamJobs_WithStatusFilter_PendingOnly` | `cmd/vmafx-controller` | B.3: PENDING filter applied correctly |
| `TestReaper_StopsOnContextCancel` | `cmd/vmafx-controller/nodes` | B.4: registry responsive after cancel |

All 30 tests in `./cmd/vmafx-controller/...` pass.

---

## Memory citation

- `project_mcp_iserror_must_be_true`: B.3 is the gRPC analogue — `return nil`
  from a server-streaming handler that sends no messages is a false-success
  signal, causing callers to silently mis-infer empty queue state.
