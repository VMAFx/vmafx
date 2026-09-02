<!-- markdownlint-disable MD013 MD060 -->
# ADR-0755: C++23 Wave 7 — drop orphan `cpu.c`, activate `cpu.cpp`

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `cpp23`, `build`, `core`, `fork-local`

## Context

Wave 5 (ADR-0735) converted `core/src/cpu.c` to `core/src/cpu.cpp` and improved the
implementation (thread-safe atomics, `constinit`). However, the rename left the original
`cpu.c` file in the tree alongside the new `cpu.cpp`, and `core/src/meson.build` continued
to list `cpu.c` in `libvmaf_cpu_sources`. The orphan `.c` was compiled rather than the
improved `.cpp`, silently negating the Wave 5 work.

This ADR records the one-file cleanup: remove `cpu.c` and update meson.build to compile
`cpu.cpp`.

## Decision

We will delete `core/src/cpu.c` and update the `libvmaf_cpu_sources` list in
`core/src/meson.build` to reference `cpu.cpp`. No other source changes are required; the
`cpu.h` header already has `extern "C"` guards, and all callers compile unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep both files, compile only `.cpp` | No deletion needed | Confusing to have both; invites regression where `.c` gets re-added | Rejected — clean deletion is unambiguous |
| Revert the `.cpp` and stay on `.c` | Zero risk | Loses Wave 5 thread-safety improvement (atomic flags) | Rejected — correctness regression |

## Consequences

- **Positive**: `cpu.cpp` is now compiled; thread-safe `std::atomic` flag initialisation
  (Wave 5 improvement) is active. Orphan confusion removed.
- **Negative**: none; the public API is unchanged.
- **Neutral / follow-ups**: Several other orphan `.c` + `.cpp` pairs likely exist (dict,
  log, mem, ref, thread_locale, etc.) from incomplete prior waves; they can be cleaned up
  in future single-file PRs.

## References

- [ADR-0735](0735-cpp23-wave5-bundle.md) — Wave 5: `cpu.c` to `cpu.cpp` original
  conversion.
- req: "Convert ONE small `core/src/*.c` file to `.cpp`."
