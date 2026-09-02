<!-- markdownlint-disable MD013 MD060 -->
# ADR-0708: C++23 Internals Pilot — `metadata_handler.c` conversion

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: build, c++, cpp23, refactor, internals, fork-local, vmafx-rebrand

## Context

The VMAFX fork's `core/src/` C sources contain patterns that cause real maintenance
footguns: manual malloc/free pairs around linked-list nodes, raw `NULL`-terminated
traversals freed in teardown functions, and no RAII to guarantee cleanup on early
returns. The public C ABI (headers under `core/include/libvmaf/`) is immutable, but
internal implementation files have no such constraint — they are never included
directly by downstream consumers.

The project already compiles C++ TUs: `svm.cpp` (vendored libsvm), all SYCL
translation units, and the Vulkan backend. Adding more C++ implementation files
is well-supported by the build system.

Research-0732 surveyed all `core/src/*.c` candidates and ranked them by ROI
(`value / (effort × risk)`). The top candidate is `metadata_handler.c` (ROI 4.0):

- 84 lines, zero float arithmetic, zero score impact.
- One linked-list struct (`VmafCallbackList` of `VmafCallbackItem` nodes) managed
  by three functions: `init`, `append`, `destroy`.
- `vmaf_metadata_destroy` walks the list and `free()`s each node — a classic
  "manual teardown that leaks if any intermediate allocation fails" pattern.
- Only one caller (`feature_collector.c`, a C file) — the `extern "C"` boundary
  is clean and minimal.

This ADR records the policy decision to convert `metadata_handler.c → .cpp` as the
Phase 4 pilot, establish the per-file conversion recipe, and validate that the
approach is safe before committing to a wider migration wave.

The Phase 4 umbrella ADR (to be filed before the pilot PR merges) sets the
project-wide C-ABI-preserving C++ modernization policy; this ADR is its first
concrete implementation.

## Decision

We will convert `core/src/metadata_handler.c` to `core/src/metadata_handler.cpp`
using C++20 idioms (`std::unique_ptr`, `constexpr`), with the following constraints:

1. The header `metadata_handler.h` gains `extern "C" { ... }` guards so all existing
   C callers (`feature_collector.c`) continue to compile and link unchanged.
2. The `meson.build` entry for the file is updated from `src_dir + 'metadata_handler.c'`
   to `src_dir + 'metadata_handler.cpp'`; a companion `cpp_sources` object with
   `override_options : ['cpp_std=c++20']` is appended to `libvmaf_sources` at the
   point where the file was previously listed.
3. Internal implementation uses `std::unique_ptr<VmafCallbackItem>` with a custom
   deleter that walks and frees the linked list, replacing the manual teardown in
   `vmaf_metadata_destroy`.
4. `constexpr` replaces any compile-time numeric constants in the file.
5. The Netflix golden gate must pass post-conversion (scores `76.668` places=4).
6. The converted file must be lint-clean under `make lint` with zero new clang-tidy
   diagnostics.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Convert `log.c` first (ROI 3.0) | Slightly smaller (72 lines) | `vmaf_log` takes `va_list` — a C++23 `std::format` replacement would change the call signature, requiring cascading header changes | Deferred to Wave 1; `metadata_handler` is cleaner |
| Convert `mem.c` first (ROI 2.0) | Trivially small (52 lines) | `aligned_malloc`/`aligned_free` are called from ~25 sites across C and C++ TUs; any signature change is high blast radius | Deferred to Wave 1 |
| Bulk convert all ROI >= 1 candidates in one PR | Faster migration | Harder to bisect if golden gate breaks; harder to review atomically | Per-file pilots reduce risk surface |
| Bump project `cpp_std` to `c++23` now | Unlocks `std::expected`, `std::format`, `std::span` | Potentially breaks SYCL / CUDA toolchain TUs not yet tested at C++23 | Deferred until >=5 files have migrated; per-target override is safe now |
| Do not convert — use C23 features instead | No new compiler dependency | C23 has `typeof`, `ckd_*`, but no RAII or `std::span` | Cannot address the RAII and type-safety goals with C alone |

## Consequences

- **Positive**: `vmaf_metadata_destroy` cannot leak nodes on any future OOM or
  early-return path because the linked-list head is `unique_ptr`-managed. Pattern
  establishes the recipe for Wave 1 (log, mem, opt).
- **Negative**: `feature_collector.c` (the only caller) must include a header with
  `extern "C"` guards — one additional include-layer. Compilation of
  `metadata_handler.cpp` requires a C++ compiler on the build host; this is already
  required by `svm.cpp` and all SYCL TUs, so no new toolchain dependency is introduced.
- **Neutral / follow-ups**:
  - Wave 1 PR: `log.c`, `mem.c`, `opt.c` (ROI 3.0, 2.0, 1.5 respectively).
  - Wave 2 PR: `ref.c`, `fex_ctx_vector.c` (ROI 1.0 each).
  - Wave 3 PR: `dict.c` (ROI 0.83; requires `std::expected` -> `cpp_std=c++23`).
  - Project-level `cpp_std=c++23` bump: after Wave 3 landing.

## References

- Research-0732 (`docs/research/0732-vmafx-cpp23-internals-migration-plan.md`).
- ADR-0692 (`docs/adr/0692-vmafx-c23-bump.md`) — bumped C standard to C23.
- ADR-0700 (`docs/adr/0700-vmafx-repo-layout.md`) — `libvmaf/` renamed to `core/`.
- `core/src/vulkan/meson.build:157` — precedent for `override_options : ['cpp_std=c++17']`.
- req: "Internal implementation moves `.c` to `.cpp` where C++23 features help: `std::unique_ptr` / RAII to replace manual `malloc`/`free` pairs."
