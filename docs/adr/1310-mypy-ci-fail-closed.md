<!-- markdownlint-disable MD013 MD060 -->
# ADR-1310: Reuse the merge-base mypy gate in required CI

- **Status**: Proposed
- **Date**: 2026-09-25
- **Deciders**: VMAFx maintainers
- **Tags**: `ci`, `python`, `tooling`, `testing`

## Context

The required `Python Lint` job installed the hash-locked mypy toolchain but ran
`mypy ai/ scripts/ || echo "mypy advisory only on first run"`. On collector
base `13aad630a`, that command checked 370 sources, reported 1,834 errors in
276 files, exited 1, and was converted to success by the shell tail. The older
ledger claim that it checked zero files is no longer true after subsequent
module-discovery repairs, but the job still enforced no type-check result.
On the rebased integration collector `8236bc821`, the same command checked 373
sources, reported 1,840 errors in 278 files, and still returned success through
the shell tail.

The local pre-push gate already solves the two constraints that made the
hosted job advisory. It checks only branch-owned tracked Python paths against
their merge-base versions, evaluates both trees under the branch's checker
configuration, excludes ambient site packages, and checks `ai/src/` separately
with `--explicit-package-bases`. Its hash-locked mypy environment therefore
does not depend on whether a runner happens to have NumPy, pandas, or PyTorch
stubs installed.

## Decision

The required `Python Lint` job will invoke
`scripts/git-hooks/pre-push-mypy.py` directly and propagate its status. It will
fetch full Git history, use `origin/master` as the pull-request baseline, and
pass the push event's exact previous commit through `VMAFX_MYPY_BASE_REF` so a
post-merge master run checks the paths that landed. The hook's local default
remains `origin/master`; its finding fingerprint, baseline configuration,
module split, and fail-closed exit semantics remain unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the advisory raw-directory command | No migration work | Every mypy failure remains green; directory discovery can regress to duplicate module identities | Does not enforce a gate |
| Install the complete AI runtime and fail on the whole tree | Maximum third-party type detail | Multi-gigabyte install, runner-dependent PEP 561 behavior, large inherited debt; the prior prototype rewrote about 280 files | Too broad and unstable for this bug fix |
| Add a second CI-only baseline/ratchet implementation | Can specialize event handling | Duplicates selection, fingerprint, cleanup, and configuration rules already regression-tested in the pre-push gate | Two authorities would drift |
| Reuse the existing merge-base gate with an explicit CI base | One implementation, hash-locked checker, new findings block, inherited debt remains actionable | CI needs full history and two mypy passes for files with findings | **Chosen** |

## Consequences

- **Positive**: New type errors in tracked `ai/` and `scripts/` Python paths
  now fail the required hosted job. Duplicate module discovery and ambient
  third-party stub drift use the same tested controls locally and in CI. Seven
  collector findings exposed during integration were repaired without
  suppressions.
- **Negative**: A finding requires a second mypy pass in a disposable
  merge-base worktree. Full Git history is fetched for this five-minute job.
- **Neutral / follow-ups**: Inherited findings remain debt rather than being
  silently reclassified as branch regressions. No model training runs, no
  dependency version changes, and no Netflix golden assertion changes.

## References

- [ADR-1261](1261-mypy-pre-push-delta-gate.md) — merge-base finding policy.
- [ADR-1282](1282-mypy-python-version-follows-requires-python.md) — checker language-version authority.
- [Research-2102](../research/2102-mypy-ci-fail-closed-2026-09-25.md) — branch audit, parent repro, and mutation evidence.
- [actions/checkout fetch-depth contract](https://github.com/actions/checkout#fetch-all-history-for-all-tags-and-branches).
- Source: `req` — “we fix everything until we cant find anything anymore for now”.
