# ADR-0858: C++23 conversion of `gpu_dispatch_env.c`

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: build, c++, cpp23, refactor, internals, fork-local, gpu-dispatch

## Context

`core/src/gpu_dispatch_env.c` (~132 LOC, 0 gotos) implements a once-snapshotted
thread-safe env-var lookup table shared by all GPU backends (CUDA, Vulkan, SYCL,
HIP, Metal). ADR-0461 specifies the contract; the original C implementation uses
`pthread_mutex_t` on POSIX and an `INIT_ONCE` / `CRITICAL_SECTION` pair on Windows,
with manual `lock_acquire` / `lock_release` helpers and a nullable `char *` to
represent "variable unset at snapshot time".

The Phase 4 C++23 internals wave (ADR-0708 pilot) established the per-file
conversion recipe: rename `.c → .cpp`, compile as an isolated static lib with
`override_options: ['cpp_std=c++23']` so the std override stays local, and
preserve every public symbol as `extern "C"`.

This file is an ideal second candidate (ROI high):

- Pure implementation file; no public header changes required.
- The Windows platform #ifdef for `CRITICAL_SECTION` / `InitOnceExecuteOnce`
  vanishes entirely — `std::mutex` is portable.
- `strdup` / nullable `const char *` replaced by `std::optional<std::string>`
  with guaranteed RAII cleanup.
- `[[nodiscard]]` on the only public entry point.
- `std::string_view` comparisons eliminate the implicit `strlen` in every
  `strcmp` call in the fast path.

## Decision

Convert `gpu_dispatch_env.c → gpu_dispatch_env.cpp` using C++23.  Compile it
as the isolated static lib `gpu_dispatch_env_cpp23_lib` in `core/src/meson.build`,
linked into `libvmaf` via `extract_all_objects`, following the ADR-0708 pattern
exactly. Remove the `.c` entry from `libvmaf_sources`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep `.c`, patch Windows path separately | Zero risk | Platform-branch boilerplate stays; missed RAII opportunity | The C++23 path is strictly simpler and already validated by ADR-0708 |
| Inline into each GPU backend dispatch_strategy.c | Eliminates a TU | Duplication; defeats ADR-0461 centralised-snapshot rationale | Contradicts ADR-0461 design |
| Use C11 `_Generic` + `pthread_once` refactor | Stays in C | No `std::optional`, no `string_view`, Windows shim remains | C cannot express the RAII semantics that make the conversion worthwhile |

## Consequences

- **Positive**: Windows `#ifdef` deleted (~25 LOC). `strdup` replaced by
  `std::optional<std::string>`. Platform mutex bootstrap removed. `[[nodiscard]]`
  enforced by the compiler.
- **Negative**: Adds one more C++ TU to the build; negligible compile overhead.
- **Neutral / follow-ups**: The `.c` source file is deleted; the `.cpp` file
  carries forward the same ADR-0461 NOLINT citations for the two `std::getenv`
  calls in the table-exhausted and slow-path branches.

## References

- [ADR-0461](0461-gpu-dispatch-env-centralised-snapshot.md) — original C
  implementation contract.
- [ADR-0708](0708-vmafx-cpp23-internals-pilot.md) — C++23 internals pilot.
- Source: user direction (task brief 2026-05-29).
