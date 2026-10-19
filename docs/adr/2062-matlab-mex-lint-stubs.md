<!-- markdownlint-disable MD013 MD060 -->
# ADR-2062: The `cpu` tidy lane measures the MATLAB MEX sources against self-authored stub headers

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: maintainer
- **Tags**: lint, clang-tidy, standards, matlab

## Context

The ten MEX sources under `compat/python-vmaf/matlab/` (STMAD and the strred pyramid tools)
include `mex.h` and `matrix.h` from the MATLAB SDK, which exists on no runner or image, and
meson never builds them. clang-tidy stopped in the preprocessor, so no lane measured them and
the files sat in the declared exception list (`.config/lint-exceptions.d/clang-tidy-coverage.toml`)
and in the `exclude_untidyable()` filter of the changed-files job. ADR-1142 applies every standard
to every file; the row `T-TIDY-MATLAB-MEX-UNMEASURED-2026-09-22` recorded the gap. The
alternative of dropping the MEX harness from the tree was weighed and not taken.

## Decision

We write minimal stub headers, `scripts/ci/lint-stubs/matlab/mex.h` and `matrix.h`, with only the
types and functions the ten files use, self-authored from the documented C API (EUPL-1.2 header, no
MathWorks text). `scripts/ci/gen-mex-compile-commands.py` appends one compile-database entry per
MEX source whose include path starts with the stubs; the `cpu` lane (`make tidy-ratchet`,
`tidy-lane.sh`) and the changed-files job run it. The stubs are a lint-only include path: no meson
target, no build and no link ever uses them. `mexErrMsgTxt` is declared `_Noreturn`, as MATLAB's
never returns. The ten exceptions and the workflow exclusion are removed; findings the lane reports
are fixed, and what remains is recorded in `tidy-baseline-cpu.json` like any other file's.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Own stub headers (**chosen**) | Every standard binds the files; the lane measures them on every runner; stubs cost 60 lines | A stub can drift from the real API (it only has to parse the ten files) | Chosen |
| Drop the MEX harness from the tree | No lint debt | Removes Netflix's training harness sources from the fork; a loss nobody asked for | Not chosen |
| Keep the exceptions | No work | An exception per file with an expiry (2027-03-31) that only moves; ADR-1142 allows no tier | Not chosen |
| Vendor MathWorks' headers | Exact | Proprietary text in an EUPL tree | Rejected |

## Consequences

- **Positive**: `ical_std.c` had `mxDestroyArray()` called on a data pointer instead of the
  `mxArray` (a real defect, fixed); the sources are free of the mechanical findings.
- **Negative**: the stubs are one more thing to keep parsing the sources (a unit test compiles every
  MEX source against them); 14 `readability-function-size` findings stay in the baseline.
- **Neutral / follow-ups**: the sources still have no execution test (nothing here runs MATLAB);
  a refactor of the oversized functions needs one.

## References

- `Q`: "Own stub headers (Recommended)" (praetor question ledger Q-016).
- [ADR-1142](1142-whole-codebase-standards.md), [ADR-0038](0038-purge-upstream-matlab-mex-binaries.md).
