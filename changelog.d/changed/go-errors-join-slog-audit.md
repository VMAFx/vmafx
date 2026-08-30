- **refactor(go):** Replaced first-error-wins cleanup patterns in
  `pkg/bisect`, `pkg/encoder`, `pkg/storage` (HTTP-serve + FUSE-mount)
  and `cmd/vmafx-controller/queue` with `errors.Join` so secondary
  cleanup failures (disk leak on encode failure, dead FUSE mount on
  readiness timeout, orphaned `rclone` subprocess, db handle leak on
  queue-init failure) surface to the caller alongside the primary
  error instead of being silently dropped. Storage helpers
  (`killProcess`, `(*FUSEMountStorage).unmount`) now return their
  failures for caller-side joining. `slog` error-attribute keys
  standardised on `"error"` across `cmd/vmafx-node/{main,server}.go`
  (was `"err"`); behaviour-neutral but unblocks dashboard joins on the
  canonical key. (ADR-0935)
