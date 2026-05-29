# ADR-0768: C++23 Wave 9 — picture_pool, gpu_picture_pool, gpu_dispatch_env

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: build, c++, cpp23, refactor, internals, fork-local, vmafx-rebrand

## Context

Continuing the ADR-0708 cpp23 migration playbook. Three small, self-contained
TUs in `core/src/` are converted from C to C++23 in this wave:

| File | Lines | Notes |
|------|-------|-------|
| `gpu_dispatch_env.c` | 132 | Once-snapshotted env-var cache; uses pthread mutex (POSIX) or CRITICAL_SECTION (Win32). |
| `gpu_picture_pool.c` | 146 | Backend-agnostic round-robin GPU picture pool (ADR-0239). |
| `picture_pool.c`     | 256 | CPU picture pool with O(1) free-list stack. |

All three files compile as C++ without functional changes; the migration
applies conservative idioms only: `nullptr` replacing `NULL`, C-header
includes replaced by their `<c…>` equivalents, `typedef struct S { } S`
replaced by `struct S { }`, and `static_cast` / `reinterpret_cast` replacing
C casts where the types require it.

### Extern "C" collateral

Five internal headers lacked `extern "C"` guards and are consumed by the
new `.cpp` TUs:

- `core/src/picture.h` — declares `vmaf_picture_ref`, `vmaf_picture_set_release_callback`, `vmaf_picture_priv_init`.
- `core/src/mem.h` — declares `aligned_malloc`, `aligned_free`.
- `core/src/ref.h` — declares `vmaf_ref_init`, `vmaf_ref_close`, etc.
- `core/src/picture_pool.h` — declares the CPU pool API (added by this PR).
- `core/src/gpu_picture_pool.h` — already had guards (ADR-0239).
- `core/src/gpu_dispatch_env.h` — already had guards (ADR-0461).

Adding `extern "C"` guards to the three headers that lacked them is a
no-op for all existing C TUs; it is necessary for the new C++ TUs so the
linker resolves unmangled symbols correctly.

### Test cleanup collateral

`core/test/meson.build` referenced `test_ansnr_simd.c` which was deleted
by the ansnr drop (PR #38 / ADR-0720) but never removed from meson. The
broken entry prevented `meson setup` on x86_64 hosts. Removed here since
this PR already touches the test build file to update `gpu_picture_pool.c`
→ `gpu_picture_pool.cpp` references.

## Decision

Convert `picture_pool.c`, `gpu_picture_pool.c`, and `gpu_dispatch_env.c`
to `.cpp` with `cpp_std=c++23` each isolated in its own static library
(same pattern as `metadata_handler_cpp20_lib` in ADR-0708).

1. Each file is renamed `.c` → `.cpp`.
2. Each is compiled via an isolated `static_library()` target with
   `override_options: ['cpp_std=c++23']` so the standard-version override
   does not leak to adjacent TUs.
3. The three libs are linked into `libvmaf` via `objects:` at the top-level
   `library()` call, replacing the previous direct-source entries.
4. `extern "C"` guards are added to `picture.h`, `mem.h`, and `ref.h`.
5. `picture_pool.h` gains `extern "C"` guards to match the pattern.
6. The orphan `test_ansnr_simd` meson entry is removed.
7. All 25 occurrences of `'../src/gpu_picture_pool.c'` in
   `core/test/meson.build` are updated to `gpu_picture_pool.cpp`.

Build: 50/50 fast tests pass. Netflix golden gate unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Wrap internal headers in `extern "C" { ... }` at the call site in the `.cpp` file | No header changes | Causes "templates with C linkage" errors when the block pulls in C++ stdlib transitively | Brittle and error-prone |
| Add `extern "C"` to public headers only | Smallest touch | Does not cover the internal (`src/`) headers consumed by these TUs | Incomplete — three internal headers still unguarded |
| Defer to a separate "add extern-C guards to all internal headers" sweep | Cleaner separation | Leaves the three new `.cpp` TUs broken until that sweep lands | Blocks this wave |

## Consequences

- **Positive**: Three more TUs migrate to C++23; `extern "C"` guards land on
  `picture.h`, `mem.h`, and `ref.h`, unblocking future cpp23 waves that
  need those declarations. Orphan meson entry cleaned up.
- **Negative**: `picture_pool.cpp` uses `reinterpret_cast` for the C-style
  "first-member is base" inheritance pattern; a future cleanup could convert
  `PooledPicturePriv` to use C++ inheritance, but that is deferred per the
  wave's "conservative idioms only" policy.
- **Neutral**: No user-visible surface change; no score impact.

## References

- ADR-0708 (`docs/adr/0708-vmafx-cpp23-internals-pilot.md`) — pilot ADR; defines the per-file conversion recipe and the isolated-static-lib isolation pattern.
- ADR-0239 (`docs/adr/0239-gpu-picture-pool.md`) — backend-agnostic GPU picture pool; documents `gpu_picture_pool`.
- ADR-0461 (`docs/adr/0461-gpu-dispatch-env.md`) — GPU dispatch env helper; documents `gpu_dispatch_env`.
- req: "cpp23 Wave 9: convert 2-3 more SMALL `core/src/*.c` files to `.cpp`. Same SAFE pattern (cpp_std=c++23 global, just rename + extern 'C' guards + conservative idioms)."
