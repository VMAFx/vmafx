<!-- markdownlint-disable MD060 -->
# AGENTS.md — cmd/vmafx-controller

Go controller service for the VMAFX distributed platform (ADR-0711, ADR-0709).
Exposes gRPC `VmafxController` (job queue + node API) and `VmafxScoring`
(direct scoring) on a single port, plus HTTP `/healthz /readyz /metrics
/v1/score`.

Per-package invariants for automated agents working in this subtree.

## Governing ADRs

| ADR | Title | Scope |
|-----|-------|-------|
| [ADR-0711](../../docs/adr/0711-vmafx-controller-impl.md) | vmafx-controller Phase 4b.1 | Go service: gRPC + HTTP, in-memory queue, persistent node registry, FIFO scheduler |
| [ADR-0961](../../docs/adr/0961-queue-pullwork-rollback-on-get-failure.md) | PullWork rollback on post-update Get failure | queue package correctness |
| [ADR-0962](../../docs/adr/0962-controller-streamjobs-and-reaper-stop.md) | StreamJobs snapshot + reaper stop signal | controller / queue / nodes correctness |

## Invariants

### queue package

1. **PullWork rollback completeness (ADR-0961)**: The three-step rollback in
   `PullWork` — SQL UPDATE to `pending`, `runningSet` delete, FIFO re-prepend —
   must remain atomic under `q.mu`.  Do not add any early-return path between
   `q.runningSet[matchID] = struct{}{}` and the `getUnlocked` call without also
   updating the rollback path in `rollbackTopending`.

2. **`getUnlockedHook` is test-only (ADR-0961)**: The `getUnlockedHook` field
   and `SetGetUnlockedHookForTest` method must not be called in production code
   paths.  The `ForTest` suffix is a hard naming contract.

3. **runningSet / pendingFIFO always consistent**: Every path that changes SQL
   job status must mirror the change in `runningSet` and `pendingFIFO`.  The
   `reload()` function is the sole recovery mechanism on controller restart and
   must remain the last line of defence, not the primary correctness mechanism.

4. **`Queue.ListAll` contract (ADR-0962)** (`queue/queue.go`):
   `ListAll(ctx, statuses)` returns a point-in-time snapshot of all jobs,
   optionally filtered by the provided status strings.  An empty `statuses`
   slice means "all statuses."  `StreamJobs` in `grpc_server.go` depends on
   this contract.  Do not change the semantics (e.g. change empty-slice
   meaning to "no jobs") without updating `StreamJobs` and its tests.

5. **`ListAll` must include `tenant_id` in its SELECT** (`queue/queue.go`):
   Both SQL queries in `ListAll` (the unfiltered and the status-filtered paths)
   must select `COALESCE(tenant_id,'')` and scan it into `job.TenantID`.
   Omitting `tenant_id` from the SELECT was a confirmed regression: `Job.TenantID`
   was always `""` in every `ListAll` result, which made it impossible for
   `StreamJobs` to surface the submitter's tenant.  Any schema migration that
   adds new columns must also be reflected in both SELECT clauses and the
   matching `rows.Scan` call.  See `queue_listall_test.go:TestListAll_TenantIDRoundTrip`
   as the regression guard.

### scheduler package

- No additional invariants yet.  Update this file when scheduler behaviour is
  formalised in an ADR.

### nodes package

1. **`nodes.NewRegistry` context signature (ADR-0962)** (`nodes/registry.go`):
   `NewRegistry(ctx context.Context, log *slog.Logger)` — the first argument
   is a **required** context.  The reaper goroutine exits when the context is
   cancelled.  Every call site must pass a real context (at minimum
   `context.Background()`).  Tests must also call `r.Close()` or cancel the
   context in a `t.Cleanup` to avoid goroutine leaks.

### grpc server

1. **`protoStatusToQueue` / `queueStatusToProto` must stay in sync (ADR-0962)**
   (`grpc_server.go`): these two conversion helpers are inverses of each
   other.  Adding a new `Job.Status` enum value requires updating both
   functions and the corresponding `queue.Status*` constant.

2. **`grpc_server_test.go` mock stream (ADR-0962)** (`grpc_server_test.go`):
   `mockStreamJobsServer` satisfies `grpc.ServerStream` explicitly (all six
   methods implemented inline).  If `grpc.ServerStream` gains new methods in
   a dependency bump, update the mock accordingly — an interface-assertion
   compile error will surface it.

### main / shutdown

1. **Shutdown ordering (ADR-0962)** (`main.go`): `nodes.NewRegistry` must be
   called *after* `observability.NewShutdownContext()` so the reaper receives
   the shutdown context directly.  `jobQueue.Close()` is deferred before
   `nodeRegistry.Close()` — preserve this order.
