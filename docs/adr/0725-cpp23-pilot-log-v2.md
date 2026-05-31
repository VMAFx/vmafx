<!-- markdownlint-disable MD013 MD060 -->
# ADR-0725: C++23 Pilot — `log.c` conversion (real C++23, supersedes ADR-0722)

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: build, c++, cpp23, refactor, internals, fork-local, vmafx-rebrand

## Context

ADR-0722 (PR #42) converted `log.c` to `log.cpp` using only C++11 features: a local
`clamp_val<T>` template helper and `constexpr std::array<const char *, 4>` level tables.
The static library was declared with `override_options: ['cpp_std=c++11']`. While
technically a `.cpp` file, the result was not meaningfully different from the original C
implementation — it carried none of the C++17/23 standard-library ergonomics that are
the stated goal of the Research-0732 Wave 1 migration plan.

The user requested a superseding PR that uses real C++23: `std::string_view` for the
level string tables (type-safe, null-pointer-safe, compile-time-sized), `std::clamp` for
bounds clamping (C++17, mandated by C++23), and `static constexpr
std::array<std::string_view, 4>` throughout. The isolated-static-lib pattern from PR #41
(`mem_cpp23_lib`, ADR-0720) is the mandatory isolation model.

## Decision

We will supersede ADR-0722 by converting `core/src/log.c` to `core/src/log.cpp` using
genuine C++23 idioms, compiled in a dedicated `log_cpp23_lib` static library with
`override_options: ['cpp_std=c++23']` — mirroring `mem_cpp23_lib` (ADR-0720) exactly.
The public C ABI (`vmaf_set_log_level` / `vmaf_log`) is preserved unchanged via
`extern "C"` guards in `log.h`. All test binaries that previously listed
`'../src/log.c'` in their source arrays now consume `log_cpp23_lib` via
`log_cpp23_lib.extract_all_objects(recursive: true)` in their `objects:` lists,
ensuring the C++23 standard override is respected for every compilation unit that
contains `log.cpp`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| C++11 with local `clamp_val<T>` (ADR-0722 / PR #42) | Compiles at project C++11 baseline; can be listed inline in test sources | No C++17/23 standard-library idioms; `const char *` arrays instead of `std::string_view`; misses the stated goal of Research-0732 Wave 1 | Superseded by this ADR |
| C++17 with `std::string_view` + `std::clamp`; project-wide `cpp_std` bump | Slightly wider feature set than actually needed | Large blast radius — affects all SYCL/CUDA TUs not yet audited at C++17; increases risk for the current build | Deferred; per-target override is the safe model until Wave 3 lands |
| C++23 isolated static lib with `override_options: ['cpp_std=c++23']` (CHOSEN) | Real C++23 features (`std::string_view`, `std::clamp`, `std::array<std::string_view>`); zero blast radius outside the lib; mirrors ADR-0720 mem_cpp23_lib | Test binaries must use `objects:` not inline sources for `log.cpp` | This is the approach taken |
| Inline test compilation with `override_options: ['cpp_std=c++23']` on each test target | Avoids the `objects:` refactor | Requires adding `override_options` to ~22 test executables; increases per-target boilerplate; diverges from the mem.cpp pattern | More invasive than the objects-extraction approach |

## Consequences

- **Positive**: `log.cpp` now demonstrates real C++23 ergonomics — `std::string_view`
  tables have compile-time-fixed size enforced by the type system; `std::clamp` is
  idiomatic and its intent is immediately clear; the isolated-lib pattern prevents any
  standard-version contamination of other TUs.
- **Negative**: Test meson.build requires an `objects:` entry for `log_cpp23_lib` in
  every test binary that needs `vmaf_log` — ~22 targets updated. This is a one-time
  cost; future log changes only touch `log.cpp`.
- **Neutral / follow-ups**:
  - ADR-0722 is superseded and its PR #42 should be closed (see PR comment).
  - Wave 1 (opt.c) is next per Research-0732; it will follow the same pattern.
  - Project-level `cpp_std=c++23` bump: deferred until Wave 3 completes (ADR-0708).

## References

- Research-0732 (`docs/research/0732-vmafx-cpp23-internals-migration-plan.md`).
- ADR-0708 (`docs/adr/0708-vmafx-cpp23-internals-pilot.md`) — pilot recipe.
- ADR-0720 (`docs/adr/0720-vmafx-drop-ansnr.md`) — mem_cpp23_lib isolation precedent.
- ADR-0722 — C++11 attempt (PR #42), superseded by this ADR.
- PR #41 — `mem_cpp23_lib` isolated static lib pattern (reference implementation).
- req: "real C++23 — std::string_view, std::clamp, std::array<std::string_view>; same isolated-lib pattern as mem.cpp from PR #41; supersedes the C++11 attempt in PR #42."
