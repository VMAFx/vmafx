# AGENTS.md — cmd/vmafx-controller

Per-package invariants for automated agents working in this subtree.

## Governing ADRs

| ADR | Title | Scope |
|-----|-------|-------|
| [ADR-0711](../../docs/adr/0711-vmafx-controller-impl.md) | vmafx-controller Phase 4b.1 | Go service: gRPC + HTTP, in-memory queue, persistent node registry, FIFO scheduler |
| [ADR-0961](../../docs/adr/0961-queue-pullwork-rollback-on-get-failure.md) | PullWork rollback on post-update Get failure | queue package correctness |

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

### scheduler package

- No additional invariants yet.  Update this file when scheduler behaviour is
  formalised in an ADR.

### nodes package

- No additional invariants yet.  Update this file when node-registry behaviour
  is formalised in an ADR.
