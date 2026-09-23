<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1273: Select language standards through warning-clean Meson preference lists

- **Status**: Proposed
- **Date**: 2026-09-20
- **Deciders**: Lusoris
- **Tags**: build, meson, c23, cpp23, windows, warnings, fork-local

## Context

ADR-1056 worked around an older MSVC/Meson mismatch by removing `cpp_std` from
Meson's built-in options and injecting `/std:c++latest` or `-std=...` through
`add_project_arguments()`. The C23 path later copied that pattern. Meson warns
for both because language-standard flags belong to the built-in `c_std` and
`cpp_std` options. The workaround also bypasses those options during compiler
feature checks, so the selected C flag had to be threaded manually through
every check.

Meson has supported an ordered list of standards since 1.3.0, and this project
already requires Meson 1.4.0. The list selects the first spelling supported by
the active compiler. That resolves the original portability problem without a
warning: GCC 14+ and current Clang select C23, GCC 13 selects its `c2x`
spelling, MSVC selects C17 (its newest Meson-supported ISO C mode), ordinary
toolchains select C++23, and MSVC selects `c++latest`. Because MSVC's C mode is
not full C23, the existing cross-platform source subset remains C17-compatible;
C23-only constructs require an equivalent MSVC path.

## Decision

Set `c_std=c23,c2x,c17` and `cpp_std=c++23,c++latest` in
`core/meson.build`'s `default_options`. Remove all direct language-standard
flags from `add_project_arguments()`. Retain a compile probe for
`std::expected` so accepting a C++23 spelling is insufficient when the paired
standard library lacks the required API. Once accepted this supersedes
ADR-1056's manual
C++ flag injection and amends ADR-0692's claim that every compiler uses C23.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Ordered built-in standard preferences plus an API probe | Warning-clean; reaches targets and Meson feature checks; preserves compiler-specific spellings | MSVC selects C17 because Meson exposes no `clatest` C choice | Chosen; it makes the actual portability boundary explicit |
| Keep direct `-std`/`/std` flags and accept or filter Meson's warning | Preserves the existing effective flags | Leaves a known warning, duplicates Meson's option machinery, and requires manual feature-check arguments | Rejected; warning suppression is not a fix |
| Force one lowest-common-denominator standard | Simple single value | C17/C++20 would remove language/library features already required by the fork | Rejected; functional regression |
| Carry a custom Meson or compiler wrapper that adds MSVC `/std:clatest` | Could preserve MSVC's newest partial C mode | Adds a build-system fork and still obscures which C23 features MSVC implements | Rejected; disproportionate maintenance cost |

## Consequences

- **Positive**: configure output no longer warns about manually supplied
  standard flags; feature checks and targets use the same selected standard;
  callers can still override either built-in option explicitly.
- **Negative**: Windows C translation units are constrained to the C17-compatible
  subset until Meson exposes a newer MSVC C standard choice. Cross-platform C23
  adoption must continue to provide an MSVC-compatible implementation.
- **Neutral / follow-ups**: the Windows build matrix remains the authoritative
  proof for MSVC selection. `std::expected` remains a hard configure-time
  requirement regardless of the C++ option spelling.

## References

- [Meson 1.3.0 release notes: multiple values for C and C++ standards](https://mesonbuild.com/Release-notes-for-1-3-0.html)
- [Meson built-in options](https://mesonbuild.com/Builtin-options.html)
- [ADR-0692](0692-vmafx-c23-bump.md) — original C23 policy, amended here for MSVC.
- [ADR-1003](1003-cpp-std-c23-bump.md) — project-wide C++23 requirement.
- [ADR-1056](1056-msvc-cpp-std.md) — the manual flag workaround this would
  supersede. It stays Accepted, and `core/meson.build` still keeps its
  trailing `none` and `/std:clatest`, until this ADR is accepted: meson's
  intel-llvm-cl backend advertises only c89/c99/c11, so a list without
  `none` aborts configure on the Windows MSVC+SYCL leg.
- Source: `req` — "there is no on touch rule anymore, no warning or error is just ignored because of being og netflix code, fix them all".
