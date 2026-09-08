<!-- markdownlint-disable MD013 MD060 -->
# ADR-1246: Model verified public functions as Cppcheck entrypoints

- **Status**: Accepted
- **Date**: 2026-09-08
- **Deciders**: VMAFx maintainers
- **Tags**: ci, build, quality

## Context

A CPU-only compile database cannot contain external application calls or every
optional backend consumer. Exhaustive Cppcheck therefore calls some exported
functions unused, including disabled-backend ABI fallbacks. At `fb793901`, 16
of 53 unused-function findings have verified `VMAF_EXPORT` declarations in
headers selected for installation. Deleting them would break that contract.
Other findings describe private, vendored or generated-fixture dependencies;
a missing caller in this profile does not establish that those are public API.

## Decision

Load one versioned Cppcheck format-2 model of exact public function names in
both local and remote analyzer commands. Add only verified exported roots,
validate them against public declarations, and preserve all existing diagnostic
categories, compile variants and failure handling. Use real-tool controls to
prove that unlisted unused functions and defects inside listed bodies still
fail. Missing or invalid model input is a failure.

## Alternatives considered

| Option | Benefit | Cost | Decision |
| --- | --- | --- | --- |
| Exact public entrypoints in one shared model | Describes external callers without changing source or disabling checks | Names require declaration review and drift checks | Selected |
| Suppress unusedFunction everywhere | Removes this class of noise | Hides genuinely unused private code | Rejected |
| Delete every function absent from the CPU call graph | Shrinks source | Breaks optional-backend and public ABI contracts | Rejected |
| Add source suppressions to every exported function | Local explanation | Duplicates one analyzer policy across backend implementations | Rejected for verified public roots |

## Consequences

- Backend scaffolds and disabled-build return contracts remain unchanged.
- Local `--enable=all` and CI's existing warning/performance/portability set
  remain distinct; this does not claim CI currently checks unused functions.
- Entry names are exact but Cppcheck does not distinguish linkage or scope:
  a same-named static function is also treated as a root. Require unique
  public names, verify declarations and retain this limitation in the controls.
- This first model covers 16 verified roots. Private helpers, vendored
  contracts and generated test consumers require separate investigation.
- Ubuntu 24.04's Cppcheck 2.13 and locally tested 2.21.1 support the model and
  existing exhaustive flag. Source compatibility is not a hosted run.
- No runtime dependency, API, backend behavior, lint baseline or required
  status-check identity changes.

## References

- req: "scaffolds or stubs arent necessarily garbage".
- req: "everything that needs to be configured in mutliple places is in global envs".
- [Research-1246](../research/1246-cppcheck-public-entrypoints.md).
- [Disabled-build contract](0374-disabled-build-enosys-contract.md).
- [Exhaustive analysis](1245-cppcheck-exhaustive-configured-analysis.md).
- [Cppcheck 2.21.1 model schema](https://github.com/danmar/cppcheck/blob/2.21.1/cfg/cppcheck-cfg.rng).
- [Cppcheck 2.21.1 unused-function analysis](https://github.com/danmar/cppcheck/blob/2.21.1/lib/checkunusedfunctions.cpp).
