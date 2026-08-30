# ADR-0768: C++23 Wave 9 — picture_pool + gpu_picture_pool

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `build`, `c++`, `cpp23`, `refactor`, `internals`, `fork-local`, `vmafx-rebrand`

## Context

Continuing the ADR-0708 cpp23 migration playbook. Two self-contained TUs
in `core/src/` are converted from C to C++23 in this wave:

| File | Lines | Notes |
| --- | --- | --- |
| `gpu_picture_pool.c` | 146 | Backend-agnostic round-robin GPU picture pool (ADR-0239). |
| `picture_pool.c` | 256 | CPU picture pool with O(1) free-list stack. |

Note: `gpu_dispatch_env.c` was included in the original Wave 9 plan but
was converted earlier and separately in ADR-0858 / PR #531. This ADR
covers only the two pool files.

All conversions apply conservative idioms only: `nullptr` replacing `NULL`,
`<c…>` headers replacing C headers, `typedef struct S { } S` replaced by
`struct S { }`, and `static_cast`/`reinterpret_cast` replacing C casts
where the types require it.

### Extern "C" collateral

Four internal headers lacked `extern "C"` guards and are consumed by the
new `.cpp` TUs:

- `core/src/picture.h` — declares `vmaf_picture_ref`,
  `vmaf_picture_set_release_callback`, `vmaf_picture_priv_init`.
- `core/src/mem.h` — declares `aligned_malloc`, `aligned_free`.
- `core/src/ref.h` — declares `vmaf_ref_init`, `vmaf_ref_close`, etc.
- `core/src/picture_pool.h` — declares the CPU pool API.

`core/src/gpu_picture_pool.h` and `core/src/gpu_dispatch_env.h` already
had guards (ADR-0239, ADR-0461).

Adding `extern "C"` guards to headers that lacked them is a no-op for
existing C TUs; it is required so the new C++ TUs resolve unmangled
symbols at link time.

## Decision

Convert `picture_pool.c` and `gpu_picture_pool.c` to `.cpp` with
`cpp_std=c++23`, each isolated in its own static library (same pattern as
`metadata_handler_cpp20_lib` in ADR-0708).

1. Each file is renamed `.c` → `.cpp`.
2. Each is compiled via an isolated `static_library()` target with
   `override_options: ['cpp_std=c++23']`.
3. The two libs are linked into `libvmaf` via `objects:` at the top-level
   `library()` call, replacing the previous direct-source entries.
4. `extern "C"` guards are added to `picture.h`, `mem.h`, `ref.h`, and
   `picture_pool.h`.
5. All occurrences of `'../src/gpu_picture_pool.c'` in
   `core/test/meson.build` are updated to `gpu_picture_pool.cpp`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Wrap internal headers at the call site | No header changes | Causes "templates with C linkage" errors when pulling in C++ stdlib | Brittle |
| Add `extern "C"` to public headers only | Smallest touch | Does not cover internal src/ headers | Incomplete |
| Defer extern-C guard sweep separately | Clean separation | Leaves the new .cpp TUs broken until sweep lands | Blocks wave |

## Consequences

- **Positive**: Two more TUs migrate to C++23. `extern "C"` guards land on
  `picture.h`, `mem.h`, and `ref.h`, unblocking future cpp23 waves.
- **Negative**: `picture_pool.cpp` uses `reinterpret_cast` for the C-style
  first-member inheritance pattern; a future pass could use C++ inheritance,
  deferred per the wave's "conservative idioms only" policy.
- **Neutral**: No user-visible surface change; no score impact.

## References

- [ADR-0708](0708-vmafx-cpp23-internals-pilot.md) — original C++23 migration
  playbook; defines the per-file conversion recipe and isolated-static-lib
  isolation pattern.
- [ADR-0239](0239-gpu-picture-pool.md) — backend-agnostic GPU picture pool.
- [ADR-0858](0858-cpp23-gpu-dispatch-env.md) — Wave 9 `gpu_dispatch_env`
  conversion (merged earlier in PR #531).
- req: "cpp23 Wave 9: convert 2-3 more SMALL `core/src/*.c` files to `.cpp`.
  Same SAFE pattern."
