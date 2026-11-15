---
paths:
  - cmd/vmafx-controller/queue/*.go
  - cmd/vmafx-controller/scheduler/*.go
  - cmd/vmafx-controller/heartbeat_cancel_test.go
  - cmd/vmafx-controller/grpc_tenant_test.go
invariant: Atomic PullWork rollback; tenant-scoped ListByTenant/PullWork/CancelledAmong; transition bool.
---
<!-- markdownlint-disable MD013 MD029 MD060 -->
# Queue and scheduler invariants (`queue/`, `scheduler/`)

## Invariants

### queue package

1. **PullWork rollback completeness (ADR-0961)**: 3-step rollback in
   `PullWork` (SQL UPDATE to `pending`, `runningSet` delete, FIFO prepend)
   atomic under `q.mu`. Do not early-return between
   `q.runningSet[matchID] = struct{}{}` and `getUnlocked` without updating
   `rollbackTopending`.
2. **`getUnlockedHook` test-only (ADR-0961)**: `getUnlockedHook` and
   `SetGetUnlockedHookForTest` prohibited in production. `ForTest` suffix
   = naming contract.
3. **runningSet / pendingFIFO consistent**: SQL job status changes mirror in
   `runningSet` and `pendingFIFO`. `reload()`: recovery on restart, not
   primary mechanism.
4. **`Queue.ListByTenant` contract (ADR-0962, ADR-1522)** (`queue/queue.go`):
   `ListByTenant(ctx, tenantID, statuses)` returns snapshot of one tenant's
   jobs, filtered by status strings. Empty `statuses` = all statuses.
   `tenant_id = ?` sits in SQL WHERE. No all-tenants read exists; never add
   one (old `ListAll` streamed every tenant's jobs). `StreamJobs` relies on contract.
5. **`ListByTenant` must include `tenant_id` in SELECT** (`queue/queue.go`):
   queries select `COALESCE(tenant_id,'')` into `job.TenantID` (`Job.TenantID` was `""`).
   Missing `tenant_id` broke `StreamJobs` tenant display. Schema additions must
   update both SELECT clauses and `rows.Scan`. Guard:
   `queue_listall_test.go:TestListByTenant_TenantIDRoundTrip`.
6. **Tenant-scoped `PullWork`, node-guarded `ReportResult` (ADR-1522)**:
   `findPendingMatch` skips other tenants' jobs. `ReportResult(ctx, Report)`
   UPDATE carries `AND assigned_node = ?`; zero rows -> `reportDecision`:
   terminal own/orphan job = idempotent nil; RUNNING job of same tenant with
   orphaned node (`Report.Orphaned`, no live session; ADR-1524 re-registered
   node) -> `adoptOrphan` compare-and-set on status + tenant, moves
   assignment; else `ErrNotAssigned`, nothing written, `runningSet` untouched.
   Never drop node guard or tenant compare. Partial reports: `MayReport`.
   Guards: `queue_tenant_test.go`, `grpc_tenant_test.go`.

7. **`Cancel` / `ReportResult` report the transition** (WP16, issue #2430):
   `(bool, error)`; true only when this call moved job to terminal state.
   Idempotent retry / already-terminal job -> false. Job metrics
   (`metrics.go::jobFinished`) count only true -> one count per job. Never
   count on nil error alone. Guard: `metrics_test.go::TestJobLifecycleMetrics`
   (repeat report + repeat cancel counted once).
8. **`Stats` = only all-tenant read, counts only** (`queue/stats.go`):
   per-tenant pending / running COUNT + MIN(created_at) and in-memory requeue
   totals (`requeued`, reasons `RequeueNodeLost` / `RequeueRestart` /
   `RequeueRollback` = `metricdef.RequeueReason` values). Returns no job row,
   no job content (ADR-1522 scopes job reads). Increment `requeued` under
   `q.mu` where a RUNNING job returns to PENDING (`RequeueNode`,
   `rollbackTopending`, `reload` count). Guard: `queue/stats_test.go`.
9. **`CancelledAmong` (ADR-1567)**: tenant + `status='cancelled'` in SQL
   WHERE, IDs bound (`repeatCommaQ`), input order kept, writes nothing.
   `Heartbeat` answers `cancel_job_ids` from it; `maxHeartbeatJobs` = 64
   (node slot limit) -> InvalidArgument above. Refused session -> ok=false,
   nothing named. Guards: `queue/cancelled_test.go`,
   `heartbeat_cancel_test.go`.

### scheduler package

- Invariants pending scheduler ADR.
