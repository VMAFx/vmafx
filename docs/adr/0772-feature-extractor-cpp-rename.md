# ADR-0772: Rename `feature_extractor.c` to `feature_extractor.cpp`

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `cpp23`, `build`, `core`, `fork-local`

## Context

PR #105 audited all remaining `.c` files in `core/src/` that are suitable for
C++ migration. `feature_extractor.c` is the registry + lifecycle manager for
every feature extractor in the library. It compiles cleanly as C++ after fixing
`void *` cast fixups on six `malloc`/`realloc` calls and guarding one implicit
`atomic_int` arithmetic conversion — the existing MSVC/GCC/Clang `atomic_int`
bridge in `feature_extractor.h` (added with the atomic-pool PR) already handles
the `<stdatomic.h>` / `std::atomic_int` divergence.

Making the file C++ is a prerequisite for future C++23 modernisations of the
pool and registry (e.g., `std::span` for the extractor list, structured
`std::expected` error returns) and aligns with the ongoing cpp23 wave series
(ADR-0735, ADR-0755).

## Decision

We will rename `core/src/feature/feature_extractor.c` to `.cpp`, apply the
minimum necessary cast fixups (six `static_cast` additions and one
`atomic_load` around an implicit arithmetic use), add `extern "C"` guards to
the function declarations in `feature_extractor.h`, and update both
`core/src/meson.build` and `core/test/meson.build` to reference the new path.
The old `.c` is deleted with `git rm`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep `.c`, add C++ wrappers | No rename | Two files; wrappers drift from definitions | Rejected — extra maintenance burden |
| Rename only, no extern "C" guard | Simpler header patch | Callers that include the header from C++ TUs would see mangled names for registry functions | Rejected — silent ABI break risk |
| Full C++23 modernisation in same PR | One pass | Large diff; harder to review; blocks this PR on all modernisations | Rejected — follow-up is better scoped |

## Consequences

- **Positive**: `feature_extractor.cpp` compiles under C++; future C++23
  modernisations of the pool/registry can proceed in smaller follow-up PRs.
  `extern "C"` guards in the header prevent accidental name mangling when
  C++ callers include it.
- **Negative**: none; public ABI is unchanged, all callers continue to compile.
- **Neutral / follow-ups**: AGENTS.md references to `feature_extractor.c` in
  `core/src/feature/`, `core/src/feature/cuda/`, and sibling AGENTS.md files
  will be updated in a doc-sweep follow-up; they are comments, not build
  artefacts, and do not block this PR.

## References

- [ADR-0735](0735-cpp23-wave5-bundle.md) — C++23 Wave 5 (cpu/ref/thread_locale).
- [ADR-0755](0755-cpp23-wave7-single-file.md) — C++23 Wave 7 (cpu.c orphan drop).
- req: "Convert `core/src/feature/feature_extractor.c` to `.cpp` per PR #105's audit."
