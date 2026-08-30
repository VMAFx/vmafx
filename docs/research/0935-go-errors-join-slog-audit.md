<!-- markdownlint-disable MD013 MD060 -->
# Research digest 0935 — Go `errors.Join` cleanup-path audit + `slog` key standardisation (2026-05-31)

## Scope

Audit of 28 fork-original Go files under `cmd/` and `pkg/` that use
`fmt.Errorf`, focused on multi-step pipelines where a primary error and
a cleanup error can both arise. The companion goal is to retire the
`"err"` vs `"error"` `slog` key drift across the Phase 4b Go binaries.

## Method

1. `grep -rIln 'fmt.Errorf' cmd/ pkg/` → 28 files.
2. For each, grep for cleanup keywords (`defer …Close`, `os.Remove`,
   `cleanup`, `Cleanup`) and the lossy patterns `_ = X()` or
   `X() //nolint:errcheck`.
3. Read each candidate to confirm an actual lost error (vs a deferred
   close on a successful path where the close-error is genuinely
   irrelevant).
4. Cross-reference `slog.X` call sites for the error key.

## Findings — lost cleanup errors (5 files, 6 sites)

| File | Site | What was lost | Fix |
|---|---|---|---|
| `pkg/bisect/bisect.go` | `Run` L245 | `os.Remove(encodedPath)` after `scoreErr` | `errors.Join` of score-err + remove-err, with `errors.Is(rmErr, os.ErrNotExist)` guard |
| `pkg/bisect/bisect.go` | `VMAFScoreFunc` defer | bare `defer os.Remove(tmpPath)` | `defer func` that logs the remove failure via `slog.Warn` |
| `pkg/encoder/encoder.go` | `runEncode` L179 | `_ = os.Remove(outPath)` after `runErr` | `errors.Join` of ffmpeg-err + remove-err |
| `pkg/storage/fuse_mount.go` | Mount-start failure path | `os.Remove(mountDir)` error after `cmd.Start` failure | `errors.Join` |
| `pkg/storage/fuse_mount.go` | Readiness-timeout path | `unmount` + `killProcess` + `os.RemoveAll` errors after `waitForPath` timeout | `errors.Join` over all four; `unmount` + `killProcess` updated to return their failures |
| `pkg/storage/http_serve.go` | Readiness-timeout path | `killProcess(cmd)` error after `waitForHTTP` timeout | `errors.Join` of readiness-err + kill-err; `killProcess` updated to return its failure |
| `cmd/vmafx-controller/queue/queue.go` | `New` × 3 init failure paths | `db.Close() //nolint:errcheck` lost the close-time error on WAL-pragma, schema, and reload failures | New `closeAndJoin(db, primary)` helper that joins the close failure into the primary error |

## Findings — `slog` key drift (2 files, 3 sites)

| File | Site | Was | Now |
|---|---|---|---|
| `cmd/vmafx-node/main.go` | L38 fatal | `"err"` | `"error"` |
| `cmd/vmafx-node/main.go` | L77 probe-warn | `"err"` | `"error"` |
| `cmd/vmafx-node/server/server.go` | L70 listener close | `"err"` | `"error"` |

Audit-verified that all other `cmd/` and `pkg/` slog call sites
already used `"error"`. No mixed-key file remains.

## Out of scope (deferred to follow-up ADR)

These call sites discard errors but at process-exit / shutdown boundaries
where there is no caller to receive a joined return value. A graceful-
shutdown contract is needed before they can be retrofitted:

- `pkg/libvmaf/libvmaf.go` — `tmpOut.Close()` immediately after create,
  bare `defer os.Remove(tmpOut.Name())`.
- `cmd/vmafx-controller/main.go` — `defer scorer.Close()`,
  `defer jobQueue.Close()` (process exit; no caller).
- `cmd/vmafx-server/main.go` — `defer scorer.Close()` (same reason).
- `cmd/vmafx-mcp/impl.go` — Python-shell-call error map uses the JSON
  payload key `"error"` for output, not a `slog` attribute; correct as-is.

## Reproducer

```bash
cd /tmp/wt-errors
go test -race -count=1 ./pkg/bisect/... ./pkg/encoder/... \
    ./pkg/storage/... ./cmd/vmafx-controller/queue/... ./cmd/vmafx-node/...
golangci-lint run --timeout=2m ./pkg/bisect/... ./pkg/encoder/... \
    ./pkg/storage/... ./cmd/vmafx-controller/queue/...
```

Expected: all tests pass; no new lint findings in the touched files
(pre-existing `errcheck` / `unused` findings in test files are
unrelated and outside the touched-file gate).

## Decision

Captured as [ADR-0935](../adr/0935-go-errors-join-slog-audit.md).
