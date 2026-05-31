<!-- markdownlint-disable MD013 MD036 MD060 -->
# ADR-0961: Controller queue — roll back PullWork on post-update Get failure (round-25 audit B.1)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: go, controller, correctness, queue, phase4b, fork-local

## Context

Round-25 scheduler-correctness audit (bug B.1) identified a permanent job-stranding
scenario in `cmd/vmafx-controller/queue/queue.go`, `PullWork`.

The sequence that triggers the bug:

1. `PullWork` removes the job from `pendingFIFO` (line 273).
2. The SQL `UPDATE jobs SET status='running', assigned_node=<nodeID>` commits
   successfully (line 277).
3. `q.runningSet[matchID]` is populated (line 287).
4. `q.getUnlocked(matchID)` is called to return the fully-hydrated job to the
   caller.  If this read fails (transient I/O error, disk full, corrupt page),
   the function returned an error at line 291 — but the in-memory and SQL states
   were not restored.

Net result: the job's SQL status is `running` with `assigned_node` set; it is
absent from `pendingFIFO`; no caller ever receives it; and it can only be
recovered by restarting the controller (which runs `reload()`, which resets
`running → pending`).

This is a correctness hole: a transient failure permanently strands a job until
the next controller restart, which may be hours or days away in a production
cluster.

The fix applies the "guess + check → act" principle from the project-wide
correctness rule (`feedback_correctness_first`) — once we confirmed the
rollback path was entirely missing, we added it rather than papering over
with a looser timeout or retry at a higher layer.

## Decision

On `getUnlocked` failure inside `PullWork`, after the SQL `UPDATE` to `running`
has committed:

1. Execute `UPDATE jobs SET status='pending', assigned_node=NULL WHERE id=?`
   to restore the SQL row.
2. `delete(q.runningSet, jobID)` to remove the in-memory running entry.
3. Re-prepend the job ID to `pendingFIFO` so the next `PullWork` call sees it.
4. Return the original `getUnlocked` error to the caller.

If the rollback SQL itself fails, log a CRITICAL-level message identifying the
job as stranded and return a wrapped error carrying both the original error and
the rollback error so operators see both in the log stream.

A `getUnlockedHook func(id string) error` field (nil in production) is added to
`SQLiteQueue` to allow tests to inject failures via `SetGetUnlockedHookForTest`.
The hook is an in-package escape hatch; it is not part of the `Queue` interface.

## Alternatives considered

| Alternative | Pros | Cons |
|---|---|---|
| Wrap the entire `PullWork` body in a SQLite transaction | True atomicity; no rollback code needed | `modernc.org/sqlite` in WAL mode serialises writes; holding a transaction open across the `getUnlocked` read adds contention; complexity of `BEGIN`/`COMMIT`/`ROLLBACK` scattered across methods |
| Accept the orphan; rely on controller restart | Zero code change | Job is unavailable until restart; unacceptable in a long-running cluster |
| Add a periodic `reload()`-style scrub | Self-healing without explicit rollback | Introduces timer goroutine + lock contention; delay is unbounded (scrub interval); root cause still present |
| Close the DB to inject test failure | Clean simulation | Causes all subsequent calls to fail; makes post-rollback assertions impossible in the same test |

The explicit rollback (chosen approach) is the minimal, correct fix: it closes
the exact gap with no new goroutines, no transaction scope creep, and a clear
audit trail.

## Consequences

**Positive**

- Jobs are never permanently stranded due to a transient post-UPDATE read
  failure.
- The CRITICAL log message provides an unambiguous operator signal when the
  rollback itself fails.
- Test coverage via `TestPullWork_GetUnlockedFailure_RollsBackToPending`
  exercises all three rollback assertions (SQL status, runningSet, FIFO).

**Negative**

- The rollback SQL adds one extra write on the failure path; this is
  acceptable because the path is already an error condition.
- The `getUnlockedHook` field is a production-visible (exported setter) test
  escape hatch; it must not be called outside tests.  The naming convention
  (`ForTest` suffix) makes the restriction clear.

**Neutral follow-ups**

- A future ADR may wrap `PullWork` in a transaction once a performance
  baseline for WAL-mode write serialisation is established.

## References

- `req`: round-25 audit bug B.1 — "After SQL UPDATE succeeds and runningSet is
  populated, getUnlocked failure returns error without rolling back SQL state or
  runningSet"
- ADR-0711: vmafx-controller Phase 4b.1 scope (original queue implementation)
- `feedback_correctness_first`: project-wide rule — investigate root cause, never
  lower thresholds or skip gates
- PR: fix(queue): roll back PullWork RUNNING state when post-update Get fails
  (round-25 audit B.1)
