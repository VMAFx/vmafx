<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1243: Allow measured scoped tightening of the lint baseline

- **Status**: Accepted
- **Date**: 2026-09-08
- **Deciders**: VMAFx maintainers
- **Tags**: ci, lint, fork-local

## Context

ADR-1142 requires cleaned files to lower their committed lint allowance. Its
whole-tree writer needs a matching full CPU build database. A focused native
fix can have a valid database for its changed translation units while lacking
the full CI toolchain and optional dependency closure. The existing `--only`
mode measures a subset but intentionally skips baseline updates; hand-editing
counts would lose the measurement guarantee.

## Decision

Allow `--only <TU>` together with `--write` to tighten exactly the requested,
actually measured translation units. Require nonempty exact coverage, successful
compilation and diagnostic parsing, matching lane/tool version, and no measured
increase in warnings or uncited suppressions. Preserve every unselected entry,
including header debt; retain original full-report metadata and record scoped
provenance. The required CI gate remains a whole-tree comparison.

Both full and scoped writers take the same nonblocking POSIX advisory lock for
the resolved baseline filename. Lock files live in a per-user temporary
directory and retain their inode; the operating system releases ownership on
process exit. Reject baseline content drift since measurement and immediately
before atomic replacement. These checks detect outside edits but cannot
serialize arbitrary editors or Git operations that ignore the lock.

## Alternatives considered

| Option | Benefit | Cost | Decision |
| --- | --- | --- | --- |
| Require a full matching measurement for every update | Complete fresh metadata | Blocks local cleanup when only a focused verified build is available | Still preferred when available |
| Replace the baseline with a partial report | Simple | Erases unmeasured debt and weakens the gate | Rejected |
| Hand-edit measured counts | Small diff | No enforced coverage, failure or regression checks | Rejected |
| Guarded scoped tightening | Preserves all unmeasured allowances and rejects increases | Requires explicit provenance for a composite baseline | Chosen |

## Consequences

- A focused fix can remove its measured allowance without changing other files.
- A scoped report cannot relax debt, clear unselected headers, or represent a
  full new tree measurement. Its original `tus` and tool metadata remain the
  last full report; scoped provenance identifies subsequent measurements.
- Empty/missing selections, failed tools, malformed diagnostics and regressions
  leave the original file unchanged. Full CI still measures all configured TUs.
- NOLINT source/header read failures are measurement failures, never zero debt.
- Baseline writes require POSIX advisory locking (the existing Linux CI and
  Linux/macOS developer lanes); measurements without writes remain portable.
  Contending writers fail with exit 5 and can retry after the owner exits.
- No third-party dependency, native runtime change or CI skip is introduced.

## References

- [ADR-1142](1142-whole-codebase-standards.md).
- [Thread-pool research](../research/thread-pool-backpressure.md).
- `req` (2026-09-08): “we are getting close to rc1, it must be close to perfect”.
