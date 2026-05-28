# ADR-0722: C++ Pilot — `log.c` conversion (Wave 1, ADR-0708)

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: build, c++, cpp11, refactor, internals, fork-local, vmafx-rebrand

## Context

ADR-0708 established the per-file C→C++ conversion recipe, piloted with
`metadata_handler.c`. Its `## Consequences` section listed `log.c` (ROI 3.0)
as the first Wave 1 target. This ADR records the conversion of `log.c` to
`log.cpp`.

`log.c` is 73 lines, has zero float arithmetic, and exposes exactly two
public symbols (`vmaf_set_log_level`, `vmaf_log`) called from roughly every
other TU in the library. The C implementation contained three maintenance
friction points:

1. Level bounds-clamping via two chained ternary expressions — intent unclear
   at a glance.
2. Designated-initialiser level tables (`const char *level_str[]`) whose
   index mapping is implicit and not enforced by the type system.
3. `enum VmafLogLevel` variable typed as the enum then compared against
   `VMAF_LOG_LEVEL_NONE` with a plain `<` — compiles cleanly but the
   directionality of the guard is easy to misread.

## Decision

Convert `core/src/log.c` to `core/src/log.cpp` using C++11 idioms available
under the project's existing baseline (ADR-0708 established C++11 as the
floor; C++17/20 features require `override_options` and break inline-in-test
compilation at the project default):

1. The header `log.h` gains `extern "C" { … }` guards so all existing C
   callers continue to compile and link without modification.
2. The `meson.build` entry is replaced by an isolated `log_cpp11_lib`
   static library (same isolation pattern as `metadata_handler_cpp20_lib`
   in ADR-0708).
3. The level tables become `static constexpr std::array<const char *, 4>`,
   making array size explicit and index semantics clear.
4. Level clamping uses a typed `clamp_val<T>` helper template (C++11,
   no `<algorithm>` std::clamp dependency) to express intent explicitly.
5. `nullptr` replaces `NULL` throughout this file.
6. `enum VmafLogLevel` in internal variable declarations is spelled as
   `VmafLogLevel` (no redundant `enum` keyword, valid in C++).
7. All test targets that inline `log.cpp` compile it at the same C++11
   default as the rest of the test executable sources.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Use C++17 `std::string_view` + `std::clamp` | More idiomatic modern C++ | Breaks inline compilation in test targets using the project's C++11 default; requires per-target `override_options` on ~25 test executables | Blast radius too large for a log file pilot |
| Use C++20 `std::format` to replace `vfprintf` | Eliminates printf format-string risks | Changes `vmaf_log` signature (fmt becomes `std::string_view`); cascading call-site edits across 50+ callers | Deferred to Wave 2 once project std bumped to C++20 |
| Add `override_options: ['cpp_std=c++17']` to every test that inlines `log.cpp` | Allows C++17 features | ~25 test-target edits; larger diff, harder to review atomically | Per-file pilot should be minimal-blast-radius |
| Convert to a C++11 `class VmafLogger` with RAII sink | Encapsulates global state | Public ABI exposes free functions; wrapping them adds indirection with zero safety gain for a two-function module | Overkill for a logging shim this small |

## Consequences

- **Positive**: Level tables are `constexpr` with compile-time-fixed size;
  the bounds check in `vmaf_log` is obviously correct against a known-size
  array. `clamp_val` helper documents the intended clamping semantics
  explicitly. Pattern establishes the Wave 1 `log.cpp` recipe for future
  maintainers.
- **Negative**: `log.cpp` requires a C++ compiler on the build host. This is
  already required by `svm.cpp`, all SYCL TUs, `metadata_handler.cpp`, and
  the Vulkan backend — no new toolchain dependency.
- **Neutral / follow-ups**:
  - Wave 1 remaining: `mem.c`, `opt.c` (ROI 2.0, 1.5 respectively).
  - When the project std is bumped to C++17 (planned after Wave 3 per
    ADR-0708), `clamp_val` can be replaced with `std::clamp` and
    `const char *` tables can migrate to `std::string_view`.

## References

- ADR-0708 (`docs/adr/0708-vmafx-cpp23-internals-pilot.md`) — conversion
  recipe and Wave 1 scope.
- Research-0732 (`docs/research/0732-vmafx-cpp23-internals-migration-plan.md`)
  — ROI ranking survey.
- ADR-0692 (`docs/adr/0692-vmafx-c23-bump.md`) — bumped C standard to C23.
- Per user direction: "internal implementation moves `.c` to `.cpp` where
  C++ features help: RAII, `constexpr`, type-safe containers."
