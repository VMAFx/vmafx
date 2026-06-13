<!-- markdownlint-disable MD013 MD060 -->
# ADR-0721: C++23 Pilot Wave 1 — `opt.c` conversion

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: build, c++, cpp23, refactor, internals, fork-local, vmafx-rebrand

## Context

Following the ADR-0708 pilot (`metadata_handler.c → .cpp`, C++20), this ADR records
the Wave 1 conversion of `core/src/opt.c` to C++23. The `opt.c` file implements
option parsing for feature extractor parameters: it converts string values (`"true"`,
`"123"`, `"0.5"`) to typed C fields via `vmaf_option_set`. Research-0732 assigned
it ROI 1.5 — moderate benefit from C++ idioms, low risk.

The original implementation had four static helpers returning bare `int` (0 or
`-EINVAL`), with `strtol`/`strtod` error paths scattered across the function body.
C++23 `std::optional<T>` cleanly models "parse succeeded → value; failed → nothing"
without any loss of performance (small, stack-only, inlinable).

The public C ABI (`vmaf_option_set`) is identical: same name, same parameter types,
same return contract (`0` on success, `-EINVAL` on error).

## Decision

We will convert `core/src/opt.c` → `core/src/opt.cpp` using C++23 `std::optional`
for the internal parse helpers, with the following constraints:

1. `opt.h` gains `extern "C" { ... }` guards so all existing C callers
   (`feature_extractor.c` and test TUs) continue to compile and link unchanged.
2. The `meson.build` entry for the file is replaced by a dedicated
   `opt_cpp23_lib` static library using `override_options : ['cpp_std=c++23']`,
   following the ADR-0708 pattern for `metadata_handler_cpp20_lib`.
3. All test executables that previously included `'../src/opt.c'` in their
   sources list now pull in `opt_cpp23_lib.extract_all_objects(recursive: true)`
   from the objects list — the same mechanism used for `metadata_handler.cpp`
   tests in ADR-0708.
4. Internal helpers (`parse_bool`, `parse_int`, `parse_double`) return
   `std::optional<T>`; nullopt maps to `-EINVAL` at the C ABI boundary.
5. `[[nodiscard]]` is added to `vmaf_option_set` for C++ callers; C callers are
   unaffected.
6. Case-sensitivity and locale behaviour of string comparisons are preserved
   exactly: `strcmp("true")` / `strcmp("false")` — no locale-aware alternatives.
7. Fast-suite test gate must pass post-conversion (50/50 tests).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `std::expected<T, int>` instead of `std::optional<T>` | Carries the error code directly; idiomatic C++23 | libc++ 17+ required; host g++ 12/13 ships only draft `<expected>` or none; would break the CI toolchain | Deferred to Wave 2 / post-toolchain-bump |
| Keep `const char *` params, no `std::string_view` | Zero diff for callers | `string_view` enables safer bounds-checked operations without runtime cost | `string_view` adopted for all read-only string params in internal helpers |
| Nullable pointer `T *out` instead of `std::optional<T>` | C-compatible return style | Heap or stack pointer; more verbose at call site; no improvement over original | `std::optional` is the idiomatic C++ form and stack-allocated |
| Bump project `cpp_std=c++23` globally | Simpler build | Risks breaking SYCL / CUDA TUs not validated at C++23 yet | Per-target override is safe now; global bump deferred per ADR-0708 rationale |
| Do not convert — keep C23 | No new compiler dependency | C23 has `ckd_*`, `typeof`, but no `optional` / RAII | Cannot meet the `std::optional` modelling goal with C alone |

## Consequences

- **Positive**: `parse_int` / `parse_double` path-of-failure is unambiguous at
  call sites — `!result` rather than inspecting errno + return value. Future
  contributors extending the type switch have a clear pattern to follow.
- **Negative**: `opt.cpp` now requires a C++23-capable compiler on the build
  host. This was already required by `svm.cpp` (C++17) and all SYCL TUs
  (C++17/20), so no new toolchain dependency is introduced.
- **Neutral / follow-ups**:
  - Wave 2 candidates: `log.c` (ROI 3.0), `mem.c` (ROI 2.0).
  - `dict.c` (Wave 3, ROI 0.83) requires `std::expected` → deferred to
    post-global `cpp_std=c++23` bump.
  - The ansnr SIMD test stub removal (test/meson.build cleanup) was bundled
    into this PR because the deleted source files were causing meson setup to
    fail — a pre-existing gap from the ADR-0720 ansnr drop (PR #38).

## References

- ADR-0708 (`docs/adr/0708-vmafx-cpp23-internals-pilot.md`) — pilot and recipe.
- Research-0732 (`docs/research/0732-vmafx-cpp23-internals-migration-plan.md`) — ROI ranking.
- ADR-0692 (`docs/adr/0692-vmafx-c23-bump.md`) — C23 bump; `cpp_std` override pattern.
- req: "Internal implementation moves `.c` to `.cpp` where C++23 features help: `std::optional` to replace raw int-return parse helpers."
