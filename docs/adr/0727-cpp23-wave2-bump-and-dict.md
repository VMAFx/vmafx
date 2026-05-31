<!-- markdownlint-disable MD013 MD018 MD060 -->
# ADR-0727: C++23 Wave 2 — project-wide `cpp_std=c++23` bump and `dict.c` → `dict.cpp`

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `build`, `c++`, `cpp23`, `refactor`, `internals`, `fork-local`, `vmafx-rebrand`

## Context

ADR-0708 established the C++ migration playbook and converted `metadata_handler.c`
to C++20 using an isolated-static-library pattern with
`override_options: ['cpp_std=c++20']`. Wave 1 PRs (#41 mem, #43 opt,
#44 fex_ctx_vector, #45 log v2) followed the same pattern at C++23, each
compiling in a per-file isolated static library rather than changing the
project-wide default.

ADR-0708 flagged `dict.c` as the highest-ROI Wave 2 candidate because it has
multiple out-param+return-code functions that map cleanly onto
`std::expected<T, int>`, and because the project-wide `cpp_std` was still
`c++11` — too old for `std::expected` — making a project-wide bump a
prerequisite.

The toolchain is already on gcc 14+ (post-ADR-0692) and clang 22, both
well above the gcc >= 13 / clang >= 16 floor required for `std::expected`
(C++23).

## Decision

We will:

1. Bump `core/meson.build` `default_options` from `cpp_std=c++11` to
   `cpp_std=c++23` project-wide. Wave 1 `override_options: ['cpp_std=c++23']`
   on the isolated libs become redundant (but harmless); cleanup is deferred.
2. Convert `core/src/dict.c` to `core/src/dict.cpp` using real C++23 idioms:
   `std::expected<T,int>` for fallible internal helpers, `std::string_view` for
   read-only string params, `std::unique_ptr` for RAII scratch buffers,
   `[[nodiscard]]` on every non-void return. Public C ABI is preserved exactly
   via `extern "C"`.
3. Rename `core/test/test_dict.c` to `core/test/test_dict.cpp` to match and
   update the source reference in `core/test/meson.build`; the white-box
   `#include "dict.cpp"` probe for the static `isnumeric()` helper is retained.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Continue per-target `override_options` (Wave 1 pattern) | No project-wide change risk | Every new C++ file needs its own isolated static lib + override; boilerplate compounds per Wave | Ergonomics cost grows; project-wide bump is the documented end-state per ADR-0708 |
| Stay at `c++20` instead of `c++23` | Slightly wider compiler compatibility floor | `std::expected` is not in C++20 | `std::expected` is the highest-ROI idiom for dict.cpp; C++23 floor is justified |
| `std::optional<T>` + errno return instead of `std::expected<T,int>` | Lower compiler floor | Error path requires out-param again; loses monadic chaining | `std::expected` directly expresses "value or error code" — the existing semantic |
| Keep `const char *` for internal params | No extra type | No bounds checking; no clear ownership signal | `std::string_view` for inputs, `std::unique_ptr` for owned buffers; `const char *` at the `extern "C"` boundary only |

## Consequences

- **Positive**: `dict.cpp` internal helpers use typed error propagation instead
  of `goto` cleanup patterns; future C++ files in `core/src/` do not need
  isolated-static-lib boilerplate to use C++23 features.
- **Negative**: Build toolchain floor rises from "any C++11 compiler" to
  gcc >= 13 / clang >= 16. The CI matrix is already gcc 14+/clang 22; no
  CI impact. External builds on old distros (Ubuntu 20.04 LTS, gcc 9) must
  upgrade or use the container image (`dev/Containerfile`).
- **Neutral / follow-ups**:
  - Wave 1 isolated-static-lib `override_options` are now redundant; cleanup
    deferred to Wave 3.
  - `test_dict.cpp` uses `#include "dict.cpp"` to white-box test the static
    `isnumeric()` helper; this is a deliberate test-only pattern documented in
    `core/AGENTS.md`.

## References

- [ADR-0708](0708-vmafx-cpp23-internals-pilot.md) — C++20/23 migration policy and Wave 1 recipe
- [ADR-0702](0702-vmafx-phase4-language-modernization.md) — Phase 4 language modernization policy
- PR #43 body note: "dict.c requires project-wide cpp_std=c++23 (std::expected)"
- req: Wave 2 delivery instruction per user direction, 2026-05-28 session
