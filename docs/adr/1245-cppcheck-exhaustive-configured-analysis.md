<!-- markdownlint-disable MD013 MD060 -->
# ADR-1245: Analyze configured Cppcheck paths exhaustively

- **Status**: Accepted
- **Date**: 2026-09-08
- **Deciders**: VMAFx maintainers
- **Tags**: ci, build, quality

## Context

A clean parser still failed local `make lint-c`: Cppcheck 2.21.1 stopped its
forward value-flow analysis after four branches and reported
`normalCheckLevelMaxBranches`. With `--enable=all` and `--error-exitcode=1`,
this coverage notice fails the gate. Suppressing the notice would preserve an
incomplete analysis while making the run appear clean. Exhaustive analysis
removes that branch budget and increases other value-flow limits; the upstream
implementation explicitly accepts a substantial runtime increase.

## Decision

Use `--check-level=exhaustive` in both the configured local driver and the
required Cppcheck workflow, preserving each existing diagnostic selection,
POSIX library model, compile-database command variant, platform/defines and
failure status. The complete configured CPU profile at `3b65d0df` completed in 93.31 seconds
with 62,472 KiB peak RSS; retain this immutable-source receipt and mandatory
real-tool positive and defect controls in CI.

## Alternatives considered

| Option | Benefit | Cost | Decision |
| --- | --- | --- | --- |
| Exhaustive analysis in both paths | Completes the affected branch traversal; preserves diagnostics | More runtime and potentially more findings | Selected; complete CPU profile measured |
| Normal analysis and suppress its budget notice | Faster and superficially green | Hides incomplete analysis | Rejected |
| Exhaustive only on selected files | Reduces expected runtime | Adds a second selection policy that may miss future complex code | Rejected |
| Keep normal analysis and its failure | No configuration change | Clean source cannot pass the required local gate | Rejected |

## Consequences

- Real defects remain failures; this does not suppress any diagnostic category
  or alter lint debt allowances.
- Longer analysis may approach the existing CI timeout. The complete CPU profile used 93.31 seconds locally; a timeout/OOM is a failed result,
  not permission to omit source variants or reduce diagnostic coverage.
- Cppcheck 2.13 (Ubuntu 24.04) and 2.21.1 both support the option. Their
  value-flow implementations differ, so passing one version is not proof of
  equivalent results or timing on another runner.
- No dependency, compiler platform, build profile or required job changes.

## References

- req: “it must be close to perfect, not a shitshow” — pre-RC1 cleanup request.
- [Research-1245](../research/1245-cppcheck-exhaustive-configured-analysis.md).
- [Configured native lint](../development/ci.md#local-lint-build-profile-and-receipts).
- [Cppcheck 2.21.1 settings](https://github.com/danmar/cppcheck/blob/2.21.1/lib/settings.cpp).
