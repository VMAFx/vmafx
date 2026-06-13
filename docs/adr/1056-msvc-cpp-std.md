# ADR-1056: Use /std:c++latest on MSVC instead of cpp_std=c++23

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `build`, `ci`, `windows`, `msvc`

## Context

ADR-1003 (PR chore/build-cpp-std-c23-bump) bumped the project-wide C++
standard default in `core/meson.build` from `cpp_std=c++11` to
`cpp_std=c++23`. Meson's MSVC backend validates the `cpp_std` option against
a hard-coded accepted-values list that does not include `c++23`; the valid
tokens are `c++11`, `c++14`, `c++17`, `c++20`, and `vc++latest`. As a result,
the `Build — Windows MSVC + CUDA` CI leg failed at configure time with:

```text
core/meson.build:1:0: ERROR: None of values ['c++23'] are supported by the
CPP compiler.  Possible values for option "cpp_std" are ['c++11', 'c++14',
'c++17', 'c++20', 'vc++latest', ...].
```

The SYCL leg was unaffected because it already passes `-Dcpp_std=c++14`
explicitly, overriding the project default before meson validates it.

## Decision

Remove `cpp_std=c++23` from the `default_options` list in `project()`.
Instead, inject the compiler flag via `add_project_arguments` inside an
`if get_option('cpp_std') == 'none'` guard: MSVC (`cxx.get_id() == 'msvc'`)
receives `/std:c++latest`; all other compilers receive `-std=c++23`.

The `get_option('cpp_std') == 'none'` guard ensures that any CI leg that
already passes an explicit `-Dcpp_std=...` override is not affected: meson
sets the option to the caller-supplied value, the guard evaluates to false, and
`add_project_arguments` is never called, avoiding a conflicting double-flag
situation. The SYCL leg's `-Dcpp_std=c++14` continues to work unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep `cpp_std=c++23` in `default_options`, add `-Dcpp_std=vc++latest` to the CUDA CI leg | Minimal change to meson.build | Adds MSVC knowledge to the workflow file; future callers need the same workaround; meson.build still advertises an unsupported default | Fragile per-leg workaround |
| Change `default_options` to `cpp_std=c++17` (lowest common denominator) | Works on all compilers without conditional logic | Loses C++23 features required by Wave 1-9 migration files (ADR-0708/0727/ADR-1003) | Regression in language-modernisation scope |
| Use `cpp_std=vc++latest` in `default_options` | Works on MSVC | GCC/Clang reject `vc++latest`; they only accept GCC-form tokens | Not portable |
| Wrap `cpp_std` selection in a `meson.options` custom option | Clean caller API | Requires meson 0.62+ options files; adds indirection | Complexity without benefit |

## Consequences

- **Positive**: `Build — Windows MSVC + CUDA` configures and compiles without
  any per-leg workaround. All Linux/macOS legs continue to build at C++23
  without change. The SYCL leg's existing `-Dcpp_std=c++14` override
  continues to work via the guard.
- **Negative**: Meson emits a `WARNING: Consider using the built-in option for
  language standard version instead of using "-std=c++23"` on GCC/Clang
  builds. The warning is informational only and does not fail the configure
  step.
- **Neutral / follow-ups**: The comment in the SYCL CI step in
  `.github/workflows/libvmaf-build-matrix.yml` was updated to reflect the new
  guard semantics.

## References

- [ADR-1003](1003-cpp-std-c23-bump.md) — cpp_std=c++23 project-default bump
  that introduced this regression.
- [ADR-0121](0121-windows-gpu-build-only-legs.md) — Windows GPU build-only
  legs rationale.
- req: user direction 2026-06-04: fix `meson cpp_std=c++23` failing on MSVC.
