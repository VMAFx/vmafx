---
paths:
  - cmd/vmafx-controller/main*.go
  - cmd/vmafx-controller/db_path*.go
  - cmd/vmafx-controller/version_flag_test.go
invariant: Handle --version before fx.New; shutdown gRPC -> queue/reaper -> scorer; never relative DB path.
---
<!-- markdownlint-disable MD013 MD029 MD060 -->
# Main entrypoint, shutdown order and state DB path

## Invariants

### main / shutdown

0. **`--version` before fx (ADR-1589)**: `isVersionRequest` prints
   `pkg/version` and returns before `fx.New`; no config, no listeners. Image
   smoke test depends on it. Guard: `version_flag_test.go`.

1. **Shutdown ordering (ADR-1119)** (`main.go`): graceful shutdown owned by
   fx lifecycle. Order: gRPC `GracefulStop` → queue `Close` + node reaper stop
   → scorer `Close`. Replaced `observability.NewShutdownContext()` / `errgroup` /
   `runHTTP`/`runGRPC`. Guard: `TestStopOrder`.
2. **Default DB path never relative** (`db_path.go`, `provideJobQueue`):
   `db.path` unset -> `vmafx/vmafx-controller.db` under `$XDG_STATE_HOME` /
   `~/.local/state` (macOS, Windows: user config dir), dir 0700; unknown ->
   startup error naming `VMAFX_DB_PATH`. Never fall back to working dir:
   old relative default committed `vmafx-controller.db{,-shm,-wal}` (#2036).
   `.gitignore` lists them. Guard:
   `db_path_test.go::TestJobQueueDefaultIsNotTheWorkingDirectory`.
