<!-- markdownlint-disable MD018 -->
# Research digest: Go static-analysis audit (2026-05-30)

## Scope

Static-analysis sweep across all fork-added Go code under `cmd/` + `pkg/`
on master tip `bbcaa8d127`, using the three industry-standard analysers
beyond the default `go vet`:

- `nilness` (`golang.org/x/tools/go/analysis/passes/nilness/cmd/nilness`)
  — possible nil-pointer dereference detection.
- `staticcheck` (`honnef.co/go/tools`) — SA-prefixed correctness rules,
  S-prefixed simplification rules, U-prefixed unused-symbol checks.
- `gosec` (`github.com/securego/gosec/v2`) — security-focused linter
  (CWE-mapped rules, e.g. G118 / G122 / G304).

Companion to PRs #330 (cmd test coverage), #347 (pkg test coverage), and
#341 (Go dependency audit). This sweep deliberately avoids those files
and avoids any `go.mod` / `go.sum` change.

## Findings (pre-fix)

| Tool        | Findings | Severity breakdown                    |
|-------------|----------|---------------------------------------|
| nilness     | 0        | (clean)                               |
| staticcheck | 13       | SA1019 ×4, U1000 ×8, S1009 ×1         |
| gosec -high | 3        | G118 ×2, G122 ×1                      |

### SA1019 (deprecated symbol) — 4 sites

`prometheus.NewGoCollector()` and `prometheus.NewProcessCollector()` have
been deprecated in favour of the `prometheus/collectors` subpackage since
client_golang v1.12 (mid-2022). Affected files:

- `cmd/vmafx-server/main.go:98-99`
- `cmd/vmafx-controller/main.go:98-99`

The replacement subpackage is already pulled in transitively; no `go.mod`
change required.

### U1000 (unused) — 8 sites

1. `cmd/vmafx-node/main_test.go` — 7 symbols (`mockController` struct,
   `newMockController`, `RegisterNode`, `Heartbeat`, `PullWork`,
   `ReportResult`, `startMockController`). The file's own header notes
   that the controller-registration path was replaced by the
   ffmpeg-probe / gRPC server model; the scaffolding was orphaned
   during that refactor.
2. `pkg/bisect/bisect_test.go` — `mockScoreFunc` helper. All current
   `TestBisect_*` tests inline their `ScoreFunc` via closures over
   `linearVMAF`, so the helper had no remaining callers.

### S1009 (redundant nil guard) — 1 site

`pkg/ai/infer_test.go:124` — `len()` on a nil slice is zero by spec, so
the `models != nil && len(models) != 0` guard simplifies to
`len(models) != 0`.

### G118 (goroutine uses Background ctx) — 2 sites (false positives)

`cmd/vmafx-server/http_server.go:182` and
`cmd/vmafx-controller/http_server.go:183` both implement the canonical
`net/http` graceful-shutdown pattern: a goroutine that waits on the
parent `ctx.Done()`, then builds a *fresh* timeout context to bound the
in-flight-request drain. Propagating the request-scoped `ctx` to
`httpSrv.Shutdown` would cancel it immediately — the very condition the
goroutine just observed — defeating the entire drain. Annotated with
`#nosec G118` + an explanatory comment.

### G122 (filepath.Walk TOCTOU) — 1 site

`cmd/vmafx-mcp/impl.go:1034` reads `.c` files under the repo's own
`core/src/feature/` (or legacy `libvmaf/src/feature/`) tree to enumerate
extractors for the MCP `list_extractors` tool. The input root is trusted
(repository-owned source), so the symlink-TOCTOU risk is minimal — but
we still added a `filepath.Abs` + `strings.HasPrefix` containment check
defending against the (hypothetical) case where the trusted root itself
contains a hostile symlink leaving the tree. Annotated with
`#nosec G122 G304` on the resulting `os.ReadFile`.

## Decisions

1. **Do not upgrade `go.mod`** — PR #341 owns dependency motion.
   Migrating to `collectors.*` is possible without a version bump because
   the subpackage is part of the same already-pinned module.
2. **Delete dead test scaffolding, do not "mark intentionally unused"
   via `_ = symbol`** — the symbols correspond to obsolete code paths
   (per the in-file comments); deleting them is the correctness fix.
3. **Annotate G118 / G122 as false positives, do not refactor** —
   restructuring graceful shutdown to use the cancelled parent context
   would introduce a *real* bug. The `#nosec` form preserves the lint
   gate's signal-to-noise for future contributors.
4. **No upstream-port impact** — every modified file is fork-original
   (`cmd/vmafx-*`, `pkg/*`); no rebase delta against Netflix master.

## Post-fix scoreboard

| Tool        | Findings |
|-------------|----------|
| nilness     | 0        |
| staticcheck | 0        |
| gosec -high | 0        |

## Verification

```bash
nilness ./...                                  # silent
staticcheck ./...                              # silent
gosec -severity high ./...                     # Issues: 0
go build ./...                                 # clean
go test -race ./cmd/vmafx-{server,controller,mcp,node}/... \
                ./pkg/{ai,bisect}/...          # all pass
```

The `cmd/vmafx-operator/internal/controller` package fails on this host
because `/usr/local/kubebuilder/bin/etcd` is not installed — pre-existing
environmental gap, unrelated to this sweep.

## References

- `req` — user direction (2026-05-30): "run Go static analysis on
  fork-added Go code in `cmd/` + `pkg/` and flag and fix real findings;
  avoid PR-overlap with #330 / #347 / #341".
- ADR-0108 — deep-dive deliverables rule (this digest discharges item 1).
- ADR-0703 — vmafx-server scope (covers the prometheus metrics surface
  that the SA1019 fix touches).
- ADR-0713 — vmafx-node scope (covers the test file whose dead mock was
  removed).
