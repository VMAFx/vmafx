# ADR-1338: Require a clean Go fix modernization gate

- **Status**: Accepted
- **Date**: 2026-09-26
- **Deciders**: Lusoris
- **Tags**: ci, go, modernization

## Context

The repository pins its Go toolchain through `go.mod`, but the required Go
workflow ran only `go vet`, `gosec`, and tests. Go 1.27's `go fix` includes
source-modernization analyzers for current standard-library APIs and language
constructs. Nothing required contributors to apply those rewrites, so a green
required check could leave the Go tree behind the pinned toolchain.

The installed Go 1.27.1 command provides `go fix -diff`: it does not write
files, prints a unified diff for any available rewrite, and exits nonzero when
that diff is non-empty. The initial repository-wide probe found actionable
rewrites across the Go module. Applying the fixers also exposed one overlapping
pair of suggestions and one interface constraint that had to be resolved before
the complete fixer set became clean; the details are recorded in
[Research-1338](../research/1338-go-fix-clean-tree-gate.md).

## Decision

Run `go fix -diff ./...` as the first Go source gate after `actions/setup-go`
and before native dependency installation or libvmaf compilation. Keep the
existing required check identity `go vet + go test`; the new step inherits the
same ADR-1140 `go_checks` impact predicate as the other heavyweight steps.

Expose two local Make targets:

- `go-fix` applies the pinned toolchain's rewrites with `go fix ./...`.
- `go-fix-check` runs the exact non-mutating CI command.

The workflow contract test pins the command, impact condition, ordering, and
both Make targets. The repository must be made clean with `go-fix` before this
gate lands; CI never writes source files.

## Alternatives considered

| Option | Benefit | Cost | Decision |
| --- | --- | --- | --- |
| Keep vet, gosec, and tests only | No new CI step | Modernization drift remains invisible | Rejected |
| Run mutating `go fix ./...` in CI | Automatically rewrites the checkout | A green job would not prove committed source is current | Rejected |
| Enable a hand-selected fixer subset | Avoids current fixer interactions | Silently misses new fixers added by the pinned Go toolchain | Rejected |
| Require `go fix -diff ./...` to be empty | Full pinned-toolchain coverage and no CI mutation | Toolchain bumps may require a reviewed source sweep | Chosen |

## Consequences

- A Go-toolchain update that introduces new safe rewrites fails closed until
  those changes are reviewed and committed.
- The check runs before the expensive native build, so modernization drift
  fails quickly.
- Contributors can apply and verify the same contract through Make.
- If two upstream fixers propose overlapping edits, maintainers resolve the
  overlap once in source and re-run the complete gate; CI is not weakened with
  a permanent analyzer exclusion.

## References

- `req` — user direction 2026-09-26: "i meant the go tool \"go fix\"? is that
  in ci? that is like the auto code update guarantee lol".
- [ADR-1238](1238-go-security-required-gate.md) — required, impact-routed Go
  validation.
- [Research-1338](../research/1338-go-fix-clean-tree-gate.md) — tool evidence,
  initial findings, and validation.
