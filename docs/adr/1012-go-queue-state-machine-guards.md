<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1012: Go queue state-machine guards — PullWork AND-status, ReportResult idempotency

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `go`, `controller`, `queue`, `correctness`, `concurrency`

## Context

Two related state-machine correctness defects in `cmd/vmafx-controller/queue/queue.go`
identified by the r3/r4 audit:

1. **`PullWork` UPDATE lacks `AND status=?` guard (line 299)** — `PullWork` dequeues
   a job from `pendingFIFO` while holding `q.mu`, then updates SQLite to `RUNNING`.
   If a concurrent `Cancel` RPC wins the SQLite write first (setting the job to
   `CANCELLED`) while `PullWork` holds `q.mu`, the subsequent `PullWork` UPDATE
   overwrites the `CANCELLED` status with `RUNNING` and the cancellation is silently
   lost.  Adding `AND status=?` with `StatusPending` makes the UPDATE conditional
   and returns zero `RowsAffected` when a race occurred.

2. **`ReportResult` UPDATE overwrites any existing status (line 378)** — A node that
   retries `ReportResult` after a transient gRPC error (server received, client
   timed out) will execute the UPDATE twice.  If a `Cancel` arrived between the two
   attempts the second UPDATE silently reinstates the result, overwriting the
   `CANCELLED` status.  Adding `AND status NOT IN ('completed','failed','cancelled')`
   makes `ReportResult` idempotent: the second call hits zero rows and logs an
   informational message.

Note: the `getUnlocked` failure rollback path was fixed in ADR-0961
(`rollbackTopending`).  This ADR closes the two remaining r3-state-machine /
r4-retry-idempotency findings for `PullWork` and `ReportResult`.

## Decision

1. Change the `PullWork` UPDATE to:
   `UPDATE jobs SET status=?,…  WHERE id=? AND status=?`
   with `StatusPending` as the second WHERE argument.  On zero `RowsAffected`,
   re-prepend the job to `pendingFIFO` and return an error so the caller retries.

2. Change the `ReportResult` UPDATE to:
   `UPDATE jobs SET status=?,…  WHERE id=? AND status NOT IN (?,?,?)`
   with `StatusCompleted, StatusFailed, StatusCancelled`.  On zero rows, log at
   INFO level and return nil (idempotent success).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Distributed lock / advisory lock | Fully serialised | Overkill; SQLite is single-writer | Rejected |
| Read-then-check before UPDATE | Simple | TOCTOU race between read and write | Rejected |
| Conditional UPDATE + RowsAffected check | Atomic, idiomatic SQLite | Slightly more code | Chosen |

## Consequences

- **Positive**: `Cancel` can never be silently overwritten by a racing `PullWork`;
  `ReportResult` retries are safe.
- **Neutral**: Callers of `PullWork` that receive the "job was cancelled before
  assignment" error are expected to retry the pull — normal queue behaviour.
- **Negative**: A `PullWork` caller that does not retry on this error class will see
  a spurious error.  The controller's own retry loop already handles this.

## References

- r3/r4 audit findings: `[r3-state-machine]`, `[r4-retry-idempotency]`
- ADR-0961 (PullWork rollback on post-update Get failure)
