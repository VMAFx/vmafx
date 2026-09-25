<!-- markdownlint-disable MD013 MD060 -->
# ADR-1322: Repository-owned MATLAB MEX lint stubs and compilation targets

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: Lusoris
- **Tags**: `lint`, `matlab`, `mex`, `clang-tidy`, `compdb`

## Context

The ten vendored MATLAB MEX sources under `compat/python-vmaf/matlab/` (`STMAD_2011_MatlabCode/ical_stat.c`, `ical_std.c` and eight `strred/matlabPyrTools/MEX/*.c`) originate from upstream Netflix research harnesses. These files include `<mex.h>` and `<matrix.h>`, proprietary headers provided only by the MathWorks MATLAB SDK.

Because MATLAB is neither installed nor licensed on developer workstations or CI environments, these translation units could not be preprocessed by `clang-tidy`, causing fatal errors (`'mex.h' file not found`). Consequently, they were excluded from the native compilation database and blanket-exempted via `exclude_untidyable()` in `.github/workflows/lint-and-format.yml`.

Row `T-TIDY-MATLAB-MEX-UNMEASURED-2026-09-22` tracked this measurement gap: whole-tree static analysis could not measure these files, and blanket exclusions concealed code health debt and potential defects.

## Decision

1. **Provide Minimal Repository-Owned Lint Stubs**:
   Create `compat/python-vmaf/matlab/include/matrix.h` and `compat/python-vmaf/matlab/include/mex.h` providing minimal C-compatible type definitions (`mxArray`, `mxComplexity`), memory management declarations (`mxCalloc`, `mxFree`, `mxDestroyArray`, `mxFreeMatrix`), data accessors (`mxGetPr`, `mxGetM`, `mxGetN`, `mxGetString`), predicate declarations (`mxIsNumeric`, etc.), and execution control functions (`mexPrintf`, `mexErrMsgTxt`).
   To accurately convey control flow to `clang-analyzer`, `mexErrMsgTxt` is decorated with `__attribute__((__noreturn__))` / `_Noreturn`.
2. **Native Meson Compilation Target**:
   Add a `matlab_mex` `static_library` target in `core/meson.build` compiling all ten translation units with `-std=gnu89 -Wno-old-style-definition -Wno-implicit-int` and required include directories. This ensures `write-compile-commands.py` exports these units to `compile_commands.json`.
3. **Incorporate into Baseline and Retire Exclusions**:
   Remove the blanket `-e '^compat/python-vmaf/matlab/'` filter from `exclude_untidyable()` in `.github/workflows/lint-and-format.yml`.
   Record the real measured debt into `scripts/ci/tidy-baseline-cpu.json` (12 translation units, 1183 warnings across 409 TUs, 0 compile failures, 0 uncited NOLINTs).
4. **Remediate Genuine Defects**:
   Fix genuine issues identified by static analysis in touched files, such as the redundant dead stores in `ical_std.c`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Delete vendored MATLAB files | Eliminates unmeasured code | Breaks backwards compatibility for callers relying on MATLAB reference harnesses | Rejected: upstream compatibility preservation |
| Keep blanket CI exclusion | Zero engineering cost | Leaves 10 C translation units completely unmeasured | Rejected: violates ADR-1142 whole-codebase standards |
| Install Octave or proprietary MATLAB SDK | Full implementation | Heavyweight external dependency; non-distributable proprietary headers | Rejected: lint stubs provide exact type and preprocessor needs without proprietary licensing |

## Consequences

- **Positive:** all ten MATLAB MEX translation units are part of the whole-tree static analysis ratchet and compilation database.
- **Positive:** `exclude_untidyable()` in CI no longer has a blanket exclusion for MATLAB code.
- **Positive:** genuine dead stores in `ical_std.c` were eliminated without altering mathematical functionality.
- **Negative:** the repository maintains minimal header stubs in `compat/python-vmaf/matlab/include/`.
- **Neutral:** Netflix golden assertions and runtime core library binaries are unchanged.
