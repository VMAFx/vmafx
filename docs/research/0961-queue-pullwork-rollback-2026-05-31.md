# Research digest: PullWork post-update rollback (round-25 audit B.1)

**Date**: 2026-05-31
**ADR**: ADR-0961
**PR**: fix(queue): roll back PullWork RUNNING state when post-update Get fails

## Summary

Round-25 scheduler-correctness audit identified a permanent job-stranding bug
in `cmd/vmafx-controller/queue/queue.go` `PullWork`.  This digest documents
the analysis, options considered, and rationale for the chosen fix.

## Bug anatomy

`PullWork` performs a multi-step sequence that is only partially rolled back on
error:

```text
1. Remove from pendingFIFO            ← in-memory
2. UPDATE jobs SET status='running'   ← SQL commit
3. runningSet[matchID] = {}           ← in-memory
4. getUnlocked(matchID)               ← SQL read
   └── if error → return err          ← BUG: steps 1-3 not reversed
```

Steps 1–3 are not reversed when step 4 fails.  The job enters a zombie state:
`running` in SQL, absent from `pendingFIFO`, not recoverable until the next
controller restart.

**Severity**: Medium-High.  Requires a transient SQLite read failure immediately
after a successful write.  In practice, disk-full or page-cache pressure on a
busy controller can trigger this.

## Verification methodology

Read the actual source file and traced the execution path manually (hypothesis
→ check → confirm).  The bug was confirmed at lines 289-291 of the pre-fix
source: no rollback code existed between `q.runningSet[matchID] = struct{}{}` and
the `return nil, fmt.Errorf(...)` on the error path.

## Fix design

Three alternatives were evaluated; see ADR-0961 §Alternatives considered for
the full pros/cons table.  The chosen approach (explicit SQL rollback + in-memory
cleanup) was selected because:

- It is minimal: no new goroutines, no transaction scope change.
- It is correct: closes the gap at the exact point of failure.
- It is observable: CRITICAL log on double-failure surfaces operator-actionable
  signal immediately.

## Test strategy

`TestPullWork_GetUnlockedFailure_RollsBackToPending` uses a call-count hook
(`SetGetUnlockedHookForTest`) to allow the first `getUnlocked` call (FIFO-scan)
to succeed while injecting failure on the second call (post-UPDATE fetch).  This
replicates the exact failure window described in the audit.

Three post-failure assertions:

1. SQL `status='pending'`, `assigned_node=NULL` (read via `Get`).
2. `RunningCount() == 0` (proxy for `runningSet` absence).
3. `PendingCount() == 1` (proxy for FIFO re-prepend).

A fourth assertion confirms that a subsequent `PullWork` — without the hook —
successfully assigns the same job, proving the retry path is functional.

## Conclusion

The fix is a 20-line targeted correction with a clear ADR audit trail and full
test coverage.  No broader architectural changes are required for this bug.
