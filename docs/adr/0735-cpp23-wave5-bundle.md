<!-- markdownlint-disable MD013 MD060 -->
# ADR-0735: C++23 Wave 5 — cpu, ref, thread_locale

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `build`, `c++`, `cpp23`, `refactor`, `internals`, `fork-local`

## Context

The ADR-0708 playbook established the pattern for incrementally converting
`core/src/*.c` files to C++23: rename to `.cpp`, wrap in an isolated
`static_library` with `override_options: ['cpp_std=c++23']`, preserve the
public C ABI via `extern "C"` in the header, and apply real C++23 idioms
rather than mechanical renames.

Wave 5 continues that migration with three small files selected by LOC
(smallest first) that have no conflicts with the in-flight Vulkan-drop
branch (#47):

- **`cpu.c` (55 LOC)**: The global CPU-flag state variables are converted
  from bare `static unsigned` to `std::atomic<unsigned>`.  This eliminates
  a potential data race when `vmaf_init_cpu()` and `vmaf_get_cpu_flags()`
  are called from different threads during initialisation, which POSIX
  permits once `pthread_once` or `std::call_once` is in play on the caller
  side.  `memory_order_relaxed` preserves near-zero overhead on the hot
  read path.

- **`ref.c` (54 LOC)**: `vmaf_ref_init` used `malloc` + `memset` +
  `atomic_init`.  Replacing with `std::make_unique<VmafRef>()` value-
  initialises the struct (eliminating the `memset`) and makes the early-
  return error paths leak-safe without any explicit `free()`.  `release()`
  transfers ownership to the raw `*ref` pointer returned to callers.
  `vmaf_ref_close` uses `delete` instead of `free` because the object was
  allocated by `operator new` via `make_unique`.  The `extern "C"` function
  declarations were also added to `ref.h`, which was the only internal
  header missing them.

- **`thread_locale.c` (132 LOC)**: The platform-specific teardown ladder
  (POSIX `uselocale`/`freelocale` vs Windows `_configthreadlocale`) is
  encapsulated in `VmafThreadLocaleState`'s destructor, replacing three
  explicit `free(state)` calls scattered across error-return paths in
  `vmaf_thread_locale_push_c`.  `vmaf_thread_locale_pop` is reduced to a
  one-liner: adopt the raw pointer into a `std::unique_ptr` and let the
  destructor run.  `std::array<char, 256>` replaces the C
  `char old_locale[256]` on Windows for bounds-safe initialisation.

## Decision

Convert `cpu.c`, `ref.c`, and `thread_locale.c` to `.cpp` with C++23
idioms.  Each is compiled as an isolated `static_library` with
`override_options: ['cpp_std=c++23']` and linked into `libvmaf.so` via
`extract_all_objects`.  The public C ABI is preserved unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep as `.c`, apply fixes in C | Zero build-system churn | Cannot use `std::atomic`, `make_unique`, RAII destructors in C; fixes would require C11 atomics (`_Atomic`) and explicit cleanup patterns that are less expressive | The goal is language modernisation, not a C-dialect band-aid |
| Single monolithic `cpp_std=c++23` override on the whole `libvmaf` target | Simpler meson.build | Risks compile failures on C TUs that mix C and C++ token rules; propagates c++23 to many files not yet reviewed | ADR-0708 established the per-file isolation pattern precisely to avoid this; Wave N converts files incrementally |
| Convert all three into one combined TU | One static lib | Loss of per-file blame history, harder to review, harder to bisect | Per-file conversion matches the Wave playbook |

## Consequences

- **Positive**: Three more files carry thread-safe atomics and RAII
  lifetime management.  `vmaf_ref_close` now uses `delete` (correct for
  `make_unique`-allocated objects) instead of `free`.  The teardown logic in
  `thread_locale` is encapsulated and cannot be accidentally bypassed by
  future early returns.
- **Negative**: Three new isolated static libs add minor build-graph
  overhead (three extra Meson targets).
- **Neutral / follow-ups**: `ref.h` now has `extern "C"` guards, which is
  the correct state for any header consumed by both C and C++ TUs.  The
  C++ standard guarantees `std::atomic<unsigned>` has the same storage
  layout as `unsigned` (same size, alignment) so the ABI of any struct that
  embeds CPU flags is unchanged.

## References

- [ADR-0708](0708-vmafx-cpp23-internals-pilot.md) — original C++20/23 migration playbook.
- Waves 1–4: `metadata_handler.cpp` (0708), plus the files listed in
  PR context for Waves 1–4.
- req: "pick 2-3 more small core/src/*.c files not yet in flight, convert
  to .cpp with real C++23 idioms" (user Wave-5 dispatch message, 2026-05-28).
