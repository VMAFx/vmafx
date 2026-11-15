---
paths:
  - cmd/vmafx-controller/backend/*.go
  - cmd/vmafx-controller/store_wiring*.go
invariant: Session UUID = node ID; River periodic lease sweep with detached ctx; /readyz 503 until running.
---
<!-- markdownlint-disable MD013 MD029 MD060 -->
# Backend seam and River lease sweep (`backend/`, ADR-2350)

## Invariants

### backend package (ADR-2350, WP17)

Seam between handlers and state: `backend.Backend`. `Postgres` = on
`store`; SQLite queue + registry behind same interface until SQLite store
lands. Wire protocol unchanged.

1. **Node ID = session ID** (Postgres): `Register` returns session UUID as
   node ID; `Report` / `MayReport` resolve attempt from session
   (`store.AttemptOf` / `RunningAttempt`), not from wire (wire fencing =
   WP8). Malformed node ID -> `ErrInvalidSession`; malformed job ID ->
   `ErrNotFound` (reads) / `ErrNotAssigned` (reports).
2. **Lease sweep = River periodic job** (`LeaseSweepKind`): leader inserts,
   any replica works it, `SKIP LOCKED` keeps sweeps apart. No
   `UniqueOpts.ByPeriod` (River refuses < 1 s; leader-only insert is
   enough). Requeues counted per process (`RecordSweep`).
3. **One-shot commands before fx** (`Command`): `migrate` (store + River
   migrations, then `CheckSchema`), `import-sqlite --from F
   [--empty-tenant T]` (read-only on F, refuses unmigrated DB, second run
   skips copied IDs). `VMAFX_DB_DSN` required. Exit 2 = usage.
4. **Backoff**: `ExponentialBackoff(base, max)`; golusoris `jobs` retry
   policy replaces it with the golusoris bump (same formula).
5. **Tests** (`go test ./cmd/vmafx-controller/backend/`, Docker): end to
   end, tenants apart, refusals, River sweep, import, commands.
   `internal/storetest` = shared DB helper (tests only). Internal: digest bump changes its consts, apidiff skips internal paths.
6. **River lifetime** (`store_wiring.go`, `riverRunner`): River started
   with own context, ended after `Stop`. Never pass fx `OnStart` ctx to
   `river.Start`: fx ends that ctx at start timeout (fxtest at start return) ->
   River stops fetching, notifier spins. Start retried in background
   (2 s, 900 tries); `/readyz` 503 until River runs (`postgresBackend.Ready`)
   -> replicas start before DB / migration Job without exiting. Guards:
   `TestRiverKeepsSweepingAfterTheStart`,
   `TestControllerStartedBeforeItsMigrationsWaitsUnready`.
