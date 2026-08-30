<!-- markdownlint-disable MD013 MD060 -->
# ADR-0935: Wrap multi-step Go cleanup paths with `errors.Join`; standardise `slog` error keys

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: @Lusoris
- **Tags**: `go`, `observability`, `refactor`, `phase4b`

## Context

The Phase 4b Go subsystems (`vmafx-controller`, `vmafx-node`, `vmafx-tune`,
`pkg/bisect`, `pkg/encoder`, `pkg/storage`) grew a large `fmt.Errorf` surface
during the rapid scaffold sprint. An audit of 28 files using `fmt.Errorf`
under `cmd/` and `pkg/` flagged two recurring antipatterns:

1. **First-error-wins multi-step pipelines.** Encode → score → cleanup
   sequences in `pkg/bisect/bisect.go`, `pkg/encoder/encoder.go`,
   `pkg/storage/fuse_mount.go`, `pkg/storage/http_serve.go`, and
   `cmd/vmafx-controller/queue/queue.go` discarded secondary errors with
   patterns like `_ = os.Remove(path)` or `db.Close() //nolint:errcheck`.
   When the cleanup itself fails (disk full, permission, dead FUSE mount,
   orphaned `rclone` subprocess) the operator gets no telemetry — the
   primary error is returned, the resource leaks silently.
2. **Inconsistent `slog` error keys.** Most call sites use
   `slog.X("msg", "error", err)`; `cmd/vmafx-node/{main,server}.go` used
   `"err"`. Log aggregators (`vector` / `vmlogs` in the dev-mcp stack) join
   on the canonical `"error"` key — the divergent files silently dropped
   structured error metadata from dashboards.

The Go 1.20 `errors.Join` primitive (released 2023) was the obvious fix
for (1); the audit deferred adopting it during the original scaffold to
keep the merge-train rolling. This ADR retires that debt.

## Decision

We will use `errors.Join` for every multi-step cleanup path in
fork-original Go code where a primary error and a cleanup error can both
arise. The pattern is:

```go
errs := []error{primary}
if cleanupErr := cleanup(); cleanupErr != nil {
    errs = append(errs, fmt.Errorf("cleanup: %w", cleanupErr))
}
return errors.Join(errs...)
```

We will standardise the `slog` error-attribute key as `"error"` across
every Go file in `cmd/` and `pkg/`. The `"err"` shorthand is retired.

`errors.Is(err, os.ErrNotExist)` is the canonical guard for cleanup
removes — a file that already does not exist is not a cleanup failure
and should not pollute the joined error.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **`errors.Join` (chosen)** | Stdlib since Go 1.20; multi-error tree visible via `errors.Unwrap() []error`; integrates with `errors.Is/As` traversal; zero new dependency | Joined errors render as newline-separated by default — log aggregators must split on `\n` to fan out | Best stdlib-native ergonomics; matches the Phase 4b "use Go 1.25 features" direction |
| `hashicorp/go-multierror` | Well-known API; `.Errors` field; predates stdlib | Adds a runtime dependency for a stdlib-resolved problem; SBOM bloat; `errors.Is/As` semantics differ subtly | No — stdlib is sufficient |
| `uber-go/multierr` | Used by zap; mature | Same dependency-bloat objection; tied to zap's ecosystem (we use `slog`) | No — same reason |
| Continue logging secondary errors via `slog.Warn` without returning them | Zero diff; keeps existing call signatures | Caller cannot programmatically branch on the cleanup outcome; operator must correlate logs across goroutines to recognise a leak; the original audit-finding stays unresolved | No — this is the status quo being retired |
| Wrap secondary errors via `fmt.Errorf("%w; cleanup: %v", primary, secondary)` | One return value; backward compatible | Loses the structured `errors.Is/As` walk for the secondary error; `%v` discards the wrap chain | No — `errors.Join` solves this properly |

## Consequences

- **Positive**: Cleanup failures (disk leak, dead mount, orphaned
  subprocess, db handle leak on init failure) are surfaced to the caller
  rather than silently dropped. Operators get the full multi-error tree
  via `errors.Unwrap() []error`. `slog` dashboards now key off a single
  canonical `"error"` attribute across every Go file in scope.
- **Negative**: Joined errors render as multi-line strings by default.
  Pretty-printers in the dev-mcp log stack need to handle the `\n`
  splitter when they want to render each error as its own row.
- **Neutral / follow-ups**: Additional `fmt.Errorf` call sites identified
  in the audit (`pkg/libvmaf/libvmaf.go` `tmpOut.Close()`,
  `cmd/vmafx-controller/main.go` `defer scorer.Close()`,
  `cmd/vmafx-server/main.go` `defer scorer.Close()`,
  `cmd/vmafx-mcp/impl.go` Python-shell error map) are out of scope for
  this ADR — they discard errors at process-exit time where the joined
  return value would have nowhere to go. A follow-up ADR may revisit
  these once a graceful-shutdown contract lands.

## References

- Go 1.20 release notes — `errors.Join`: <https://go.dev/doc/go1.20#errors>
- ADR-0711 — Phase 4b.1 controller scope expansion
- ADR-0713 — Phase 4b vmafx-node Go worker
- ADR-0719 — vmafx-node rclone integration (storage cleanup paths)
- Source: paraphrased per user direction — the modernization audit
  called out the first-error-wins pattern in
  bisect/ladder/node-executor pipelines as the highest-value Go cleanup
  target and asked for an `errors.Join` sweep plus `slog` key
  standardisation.
