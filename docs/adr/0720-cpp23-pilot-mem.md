<!-- markdownlint-disable MD013 MD060 -->
# ADR-0720: C++23 Wave-1 Pilot — `mem.c` conversion

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: build, c++, cpp23, refactor, internals, fork-local, vmafx-rebrand

## Context

ADR-0708 established the per-file C++ conversion recipe by converting
`metadata_handler.c → .cpp` (C++20, ROI 4.0).  The same ADR documented a Wave-1
follow-up covering `mem.c` (ROI 2.0), `log.c` (ROI 3.0), and `opt.c` (ROI 1.5).

`mem.c` provides two functions — `aligned_malloc` and `aligned_free` — called from
approximately 25 sites across `core/src/`. The file is minimal (53 lines, zero float
arithmetic, zero score impact) but gains two concrete C++23 improvements:

1. `[[nodiscard]]` on `aligned_malloc` propagates ignored-return warnings to every
   C++ call site at zero ABI cost (the attribute is transparent to C callers).
2. `_POSIX_C_SOURCE` feature-test macro (needed in C to expose `posix_memalign`) is
   eliminated — C++ compilers expose `posix_memalign` via `<cstdlib>` without it.

The public C ABI (`aligned_malloc`, `aligned_free`) is unchanged.  Zero-on-allocate
and poison-on-free semantics are preserved exactly: `posix_memalign` /
`_aligned_malloc` return uninitialised memory; `free()` / `_aligned_free` do not zero
memory.  Callers requiring zero initialisation must call `memset` themselves.

A pre-existing meson bug was fixed in the same PR: `core/test/meson.build` still
referenced `test_ansnr_simd.c` (and all ansnr source files), which were deleted when
the ansnr feature was dropped.  The stale `executable()` and `test()` blocks blocked
`meson setup` from completing; they were removed as a touched-file cleanup
(CLAUDE.md §12 r12).

## Decision

Convert `core/src/mem.c` to `core/src/mem.cpp` with `cpp_std=c++23`, following the
ADR-0708 isolation pattern:

1. `mem.h` gains `extern "C" { … }` guards so all ~25 C callers compile and link
   unchanged.
2. `mem.cpp` replaces the `_POSIX_C_SOURCE` define with `<cstdlib>` / `<cstddef>`,
   adds `[[nodiscard]]` to `aligned_malloc`, replaces `NULL` with `nullptr`.
3. `core/src/meson.build`: a `mem_cpp23_lib` static library (pattern mirrors
   `metadata_handler_cpp20_lib`) compiles `mem.cpp` with
   `override_options : ['cpp_std=c++23']`.  `mem.c` is removed from
   `libvmaf_sources`; `mem_cpp23_lib.extract_all_objects(recursive: true)` is added
   to the final `library()` objects list.
4. `core/test/meson.build`: all 24 `'../src/mem.c'` references updated to
   `'../src/mem.cpp'`; stale ansnr test blocks removed.
5. Zero-on-allocate / poison-on-free / alignment-pass-through semantics preserved
   bit-for-bit.
6. Netflix golden gate must pass post-conversion (scores `76.668` places=4).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `std::unique_ptr<void, AlignedDeleter>` wrapper | RAII for aligned buffers | Blast radius: all ~25 call sites switch to non-pointer type; C callers lose raw pointer | High risk, low gain for a two-function file |
| `std::span<std::byte>` return type | Packages pointer+size together | Changes public ABI (`void*` → `span`); C callers break | ABI preservation is non-negotiable |
| Exceptions at boundary (throw `std::bad_alloc`) | Idiomatic C++ OOM signalling | All ~25 callers are C and would hit `std::terminate` on uncaught throw | Return-`nullptr` on failure is the existing contract; preserve it |
| Keep as `mem.c` with C23 `[[nodiscard]]` | No C++ compiler needed | C23 `[[nodiscard]]` is not yet in every toolchain; `_POSIX_C_SOURCE` still required | Per ADR-0708 recipe: move to `.cpp` for C++23 opt-in |
| Bulk `mem.c + log.c + opt.c` in one PR | Faster Wave-1 migration | Harder to bisect if a test breaks; per ADR-0708 policy: one file per pilot | Per-file pilot reduces review surface |

## Consequences

- **Positive**: `[[nodiscard]]` on `aligned_malloc` catches ignored-return at C++
  call sites at compile time.  `_POSIX_C_SOURCE` noise removed.  Wave-1 recipe
  validated for `log.c` and `opt.c` (Wave-1 remainder).
- **Negative**: `core/test/meson.build` and `core/src/meson.build` must list
  `mem.cpp` rather than `mem.c` — any upstream sync that adds new test executables
  sourcing `../src/mem.c` will need to be updated to `.cpp`.
- **Neutral / follow-ups**:
  - Wave-1 remainder: `log.c` (ROI 3.0), `opt.c` (ROI 1.5).
  - Wave 2: `ref.c`, `fex_ctx_vector.c` (ROI 1.0 each).
  - Wave 3: `dict.c` (ROI 0.83; requires `std::expected` → `cpp_std=c++23` on that file,
    already available with per-target override).

## References

- Research-0732 (`docs/research/0732-vmafx-cpp23-internals-migration-plan.md`).
- ADR-0708 (`docs/adr/0708-vmafx-cpp23-internals-pilot.md`) — pilot recipe and ROI table.
- ADR-0692 (`docs/adr/0692-vmafx-c23-bump.md`) — bumped C standard to C23.
- ADR-0700 (`docs/adr/0700-vmafx-repo-layout.md`) — `libvmaf/` renamed to `core/`.
- req: "Internal implementation moves `.c` to `.cpp` where C++23 features help: `std::unique_ptr` / RAII to replace manual `malloc`/`free` pairs."
