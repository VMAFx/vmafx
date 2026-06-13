# ADR-1080: UBSan enum-invalid-value fixes in vmaf_log and vmaf_option_set

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `ci`, `sanitizer`, `build`

## Context

The ASan + UBSan PR gate (`sanitizers.yml`) was failing on two tests that
deliberately pass out-of-range enum values to production code:

- `test_log` calls `vmaf_log(VMAF_LOG_LEVEL_NONE - 1, ...)` to verify the
  function silently discards sub-zero severity levels.  `VMAF_LOG_LEVEL_NONE`
  is 0, so the argument is -1, which as an unsigned enum is 4294967295 —
  not a named VmafLogLevel enumerator.  UBSan's `enum` check fires at
  `log.cpp:120` when that value is loaded through the enum type in a
  comparison.

- `test_opt` calls `vmaf_option_set` with `opt->type = (enum VmafOptionType)9999`
  to verify that unknown option types return `-EINVAL`.  The `switch(opt->type)`
  at `opt.cpp:105` loads 9999 through the enum type; UBSan fires because 9999
  is not a representable VmafOptionType enumerator.

Both tests are correct: gracefully rejecting invalid inputs is load-bearing
behaviour, and these tests guard against regressions.  The bug is in the
production code, not the tests: comparisons and switch dispatches on C enums
whose integer value falls outside the named enumerator set are undefined
behaviour under the C++ standard (and C via `-fsanitize=enum`).

The ASan + UBSan job runs on every PR; the TSan job runs on master pushes
only and does not catch UBSan issues.  This made the failures invisible on
master pushes while blocking every PR that ran the gate.

## Decision

In `vmaf_log`, cast the `level` parameter to `int` before all comparisons
(`level_int <= VMAF_LOG_LEVEL_NONE`, `level_int > vmaf_log_level`, and the
array index computation).  The int cast is value-preserving for both valid
and invalid inputs and eliminates the UB without changing observable
semantics.

In `vmaf_option_set`, cast `opt->type` to `int` before the `switch`.  The
case labels are integer constants; `switch(int)` is semantically identical
to `switch(enum)` when the enum's underlying type is int (the default in
C/C++ for unscoped enums without a fixed underlying type).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Deselect `test_log` and `test_opt` from the ASan+UBSan run | No code change required | Silences real UB; loses regression coverage for graceful invalid-input handling | Deselects are for tests that are structurally incompatible with the sanitizer runtime (huge-alloc, RLIMIT_AS), not for tests that expose genuine UB in production code |
| Change the tests to avoid invalid enum values | Tests still pass | Removes coverage of the exact production path callers will hit if they pass bad values; the tests are correct | Tests are not wrong — production code must not invoke UB on invalid input |
| Add `[[clang::no_sanitize("enum")]]` to the two functions | Local opt-out | Loses UBSan coverage for the rest of each function; future enums in the same function would silently escape detection | Too broad; int-cast is surgical |

## Consequences

- **Positive**: `test_log` and `test_opt` pass under UBSan; the ASan+UBSan
  PR gate is green on all branches.  Production code is UB-free on
  invalid-enum inputs.
- **Negative**: None.  The generated code is identical; both enum types have
  `int` as their underlying type so the cast is a no-op at the ABI level.
- **Neutral / follow-ups**: Any future comparison or switch on `VmafLogLevel`
  or `VmafOptionType` should follow the same pattern when the caller may
  supply out-of-range values.

## References

- `req`: The user directed investigation of the UBSan failure on the master
  push sanitizer gate and fixing the root cause.
