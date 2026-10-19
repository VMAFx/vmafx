<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-2170: A CI leg that prints no warnings turns them into errors, one leg at a time

- **Status**: Accepted
- **Date**: 2026-10-07
- **Deciders**: Lusoris
- **Tags**: `ci`, `build`, `lint`

## Context

HISS-10 asks for zero warnings across compiler, linter and format sweeps, and the maintainer decision recorded as Q-061 extends it to every language. On the master push `70d6dd0a5`, 39 of 103 CI jobs printed compiler or linker warnings: 71,600 MSVC warnings per Windows job (owned by the MSVC lane), and for every other toolchain a small set of source sites repeated once per translation unit that includes them (`-Wmissing-field-initializers` in option tables and tests, `-Wreorder-init-list` in the Metal tables, `-Wmismatched-tags`, `-Wimplicit-fallthrough`, `-Wunused-function` behind `#if`, MinGW `-Wattributes` from `VMAF_EXPORT`, icx `-Woverriding-option` on every compile). 230 distinct (file, line, flag) sites account for roughly 4,000 warnings outside MSVC. A leg that is clean but not gated drifts back: a new warning is only noticed when someone reads a log.

## Decision

Each non-MSVC leg is first brought to zero warnings with fixes that change no computed value and suppress nothing (no `-Wno-*`, no `#pragma ... ignored`, no flag removed to hide a class). The sites are counted as unique (file, line, flag) before and after.

Then that leg turns warnings into errors in one place: `-Dwerror=true` in the leg's `meson_extra` of the CI workflow for the compilers, and the linker's own switch (`-Wl,--fatal-warnings` for GNU ld, lld and MinGW, `-Wl,-fatal_warnings` for ld64) in `-Dc_link_args` / `-Dcpp_link_args` of the same string. Rust keeps `cargo clippy -- -D warnings`. Release and container images are not built with `werror`: a compiler newer than the one the leg pins must not stop a release over a new diagnostic; the CI legs pin the compiler they gate.

A leg that is not yet at zero stays ungated and is listed in `docs/development/ci.md` with its count and the cause, so the remaining debt is visible. A diagnostic that is a defect of the tool (not of our code) needs a declared exception: one file, one rule, a reason and an expiry.

Each gate ships a negative case: a planted warning fails the leg's toolchain once (recorded in the PR), and the positive run is kept next to it.

Two spellings change so that a clean build is possible without suppression, each with a value-preservation proof:

- icx: `-fp-model=precise -ffp-contract=off` becomes `-fp-model=precise -fno-fast-math -fcomplex-arithmetic=full -ffp-contract=off`. The old pair is the override icx reports as `-Woverriding-option` on every compile (7,300 times on the Linux Intel LLVM leg alone). Compiled both ways, the clang `-###` argument lists are equal apart from the spelled `-fcomplex-arithmetic=full` token, the predefined macros are equal, and the objects are byte-identical (see the PR for the corpus run).
- `VMAF_EXPORT` is empty for GCC on MinGW: the PE target has no ELF visibility, the attribute was already ignored, and under LTO it drew a warning per use.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `-Werror` for every build from `core/meson.build` | one switch | breaks release and distro builds on the first diagnostic of a newer compiler; hides which leg is clean | A leg gates itself in CI; release builds stay unaffected |
| Per-warning `-Wno-*` / `#pragma` suppression | fast | the debt stays; contradicts HISS-10 and Q-061 | Fix each site |
| `-ffp-model=strict` for icx | no override warning | adds strict exception behaviour and rounding-mode assumptions; changes code | Not value-preserving |
| `link_language` on each Metal target | fixes the cause per target | several hundred executables; Meson 1.12 still names `-lc++` twice for mixed Objective-C++ links | `-Wl,-no_warn_duplicate_libraries` on the Metal links, with the reason beside it (maintainer decision 2026-10-07) |
| One gate PR for all legs at once | simple | a leg that is not at zero blocks the rest | Legs gate as they reach zero |

## Consequences

- **Positive**: a new warning on a gated leg fails the pull request that introduces it; the list in `docs/development/ci.md` shows what is left.
- **Negative**: a compiler upgrade on a gated leg can fail the leg until the diagnostic is fixed; the pin of that leg's compiler is part of the gate.
- **Neutral / follow-ups**: the Windows MSVC and icx-cl legs follow the MSVC lane; the macOS Metal leg is gated with ld64's duplicate-library warning switched off for its links (Meson 1.12 emits the duplicate); the deliberate spill probe of the SYCL self-test compiles through `core/src/sycl/run_captured.py`.

## References

- Q-061 (maintainer decision, warnings are errors in every language) and the orchestrator brief `all-legs-zero-warnings-2026-10-07`: "every non-MSVC CI leg ... at 0 warnings with value-preserving fixes and no suppression, then warnings-as-errors per leg with a proven planted check".
- [ADR-1142](1142-whole-codebase-standards.md), [ADR-1461](1461-strict-fp-every-translation-unit.md), [ADR-1367](1367-sycl-strict-fp-every-feature-tu.md).
