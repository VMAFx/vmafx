<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1050: Go surface coverage push — vmafx-mcp arg helpers, model enumeration, fallback paths

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `go`, `mcp`, `coverage`, `test`

## Context

The Go coverage scout report (2026-06-04) identified `cmd/vmafx-mcp` as the lowest-coverage
Go package at ~8%. All 13 tool handler functions in `impl.go` were at 0% coverage. The
root cause is that the handlers dispatch to real external binaries (`vmaf`, `ffprobe`,
`vmaf-tune`), making full integration coverage impractical in unit tests. However, several
paths are exercisable without a real binary:

- The five arg-helper functions (`strArg`, `intArg`, `floatArg`, `boolArg`, `hasArg`) —
  pure logic, 100% testable.
- `handleVmafScore` path-validation error paths — reject empty ref/zero dimensions before
  reaching the binary.
- `handleListModels` — walks the filesystem; testable with a temp directory.
- `handleListBackends` and `handleVmafVersion` — fallback paths when binary is absent.
- `probeBackends` — cache population and cache hit with a non-existent binary path.
- `stripModelExt`, `handleDescribeModel` empty-name guard.

The vmafx-node `executor.go` `classifyJob` function and nil-scorer/nil-aiReg error paths
are also purely logic-based and were added to `executor_test.go`.

## Decision

Add `cmd/vmafx-mcp/impl_test.go` (23 tests) and `cmd/vmafx-node/executor_test.go` (6 tests)
covering the testable-without-binary paths. The intent is to raise the vmafx-mcp coverage
floor from ~8% to an estimated 35–45% on the covered functions, establishing a baseline for
further coverage PRs as mock infrastructure is added.

Full handler coverage (which requires mocking `exec.CommandContext`) is deferred to a
subsequent PR when the team has established a consistent mock pattern for external binaries.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Integration tests against real binary | 100% handler coverage | Requires vmaf binary in CI; adds flakiness on machines without GPU | Deferred to a follow-up mock-based approach |
| No coverage push | Nothing to break | ADR-0922 floor applies to Go surface eventually | Not acceptable |
| Mock `exec.CommandContext` now | Better coverage | Requires a mock injection point (refactor impl.go) | Deferred; the tested paths already exercise the non-binary code paths |

## Consequences

- **Positive**: vmafx-mcp and vmafx-node executor get meaningful unit-test baselines.
- **Negative**: Handler bodies remain uncovered until binary mock infrastructure is added.
- **Neutral / follow-ups**: A follow-up PR can add `testutil.NewFakeVmafBin(t)` that writes
  a shell script responding to `--help`, `--version`, and VMAF JSON output, enabling full
  handler coverage without a real GPU.

## References

- Go coverage scout report (2026-06-04).
- `cmd/vmafx-mcp/impl.go`, `cmd/vmafx-node/executor.go`.
