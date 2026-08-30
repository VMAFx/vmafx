<!-- markdownlint-disable MD013 MD060 -->
# ADR-0723: C++23 Pilot — `fex_ctx_vector.c` Conversion (Wave 2)

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: build, c++, cpp23, refactor, internals, fork-local, vmafx-rebrand

## Context

ADR-0708 established the policy for converting `core/src/*.c` implementation files
to C++23, piloted on `metadata_handler.c` (ROI 4.0). That ADR's Consequences section
scheduled `fex_ctx_vector.c` for Wave 2 (ROI 1.0).

`fex_ctx_vector.c` is the registration and lifetime manager for the
`RegisteredFeatureExtractors` vector — the data structure that holds every
`VmafFeatureExtractorContext *` registered for a scoring run. It is called from
`libvmaf.c` and `predict.c` (C files) and exercised directly by
`test_feature_extractor.c`. Converting it to C++23:

- Eliminates the manual `realloc`/`NULL`-fill grow path using `std::vector` as a
  reservation helper inside `init`.
- Removes raw `malloc`/`free` from `destroy` by allowing RAII scoping where the
  growth semantics can be expressed idiomatically.
- Validates the `extern "C"` + pre-`<atomic>` include pattern required when a C++
  TU consumes internal C headers that use `atomic_int` (`feature_extractor.h`).

The conversion also uncovered and fixed a stale meson.build reference to
`test_ansnr_simd.c` left behind by the ADR-0720 ansnr feature drop — touching the
test `meson.build` triggered the fix per CLAUDE.md §12 r12 (lint clean all touched
files).

## Decision

We will convert `core/src/fex_ctx_vector.c` to `core/src/fex_ctx_vector.cpp` compiled
with `override_options: ['cpp_std=c++23']` as an isolated static library
(`fex_ctx_vector_cpp23_lib`), linked into the final `libvmaf` via
`extract_all_objects()`, following the ADR-0708 pattern. Constraints:

1. `fex_ctx_vector.h` gains `#ifdef __cplusplus` / `extern "C"` guards so all C
   callers (`libvmaf.c`, `predict.c`) compile and link unchanged.
2. `feature_extractor.h` is updated to use `#include <atomic>` +
   `using std::atomic_int` for all C++ modes (not just MSVC), preventing
   `extern "C"` block / `<atomic>` template-in-C-linkage conflicts when any C++ TU
   includes the header.
3. The capacity-doubling growth strategy (`capacity * 2`) is preserved exactly —
   no capacity-policy change.
4. Insertion order is preserved exactly.
5. All exceptions are contained within the TU; the public functions return C `int`
   error codes at the `extern "C"` boundary.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `std::vector<VmafFeatureExtractorContext *>` as the struct field (replacing raw `**`) | Cleanest C++ idiom; RAII-managed | Changes the ABI-visible `RegisteredFeatureExtractors` layout; breaks every C caller that reads `rfe->fex_ctx` / `rfe->cnt` / `rfe->capacity` directly | C ABI must be preserved — struct fields are read by callers in other TUs |
| Use `std::vector` as RAII helper in `init` only, keep raw pointer management for `append`/`destroy` | Validates the include-pattern; preserves growth strategy exactly | Slightly asymmetric — `init` uses vector for reserve, `append` uses `realloc` | Chosen — best trade-off: RAII where it adds value, C-compatible raw pointer for the struct's lifetime |
| `std::span` for read-only view in `append` | More expressive iteration | `RegisteredFeatureExtractors` struct uses `unsigned` `cnt`/`capacity`; `span` construction from raw pointer + `unsigned` is safe but adds a cast | Deferred to Wave 3 when `cpp_std=c++23` is project-wide and the span helper is universally available |
| Preserve MSVC-only `<atomic>` bridge in `feature_extractor.h` | Minimal change surface | Breaks any non-MSVC C++ TU that includes `feature_extractor.h` inside `extern "C"` (templates-in-C-linkage error on GCC/Clang) | Extended bridge to all C++ modes — zero cost for C callers, fixes the class of error permanently |
| Wrap `feature_extractor.h` include in `#pragma push_macro` hacks | Avoids header change | Fragile, non-portable | Not considered seriously once the `<atomic>` bridge extension was validated |
| Do not convert — use C23 `_Static_assert` / `typeof` instead | No new compiler dependency | Cannot address RAII, `std::vector`, or `std::span` goals | C alone cannot provide the target ergonomics (ADR-0708 rationale inherited) |

**Downstream leverage rationale:** `fex_ctx_vector.cpp` establishes the
`extern "C" { #include ... }` + pre-`<atomic>` include pattern needed by any future
C++ TU that consumes `feature_extractor.h`. Without this pattern being validated and
documented here, each subsequent extractor conversion (Wave 3+) would rediscover the
same `atomic_int`-in-`extern "C"` link error. By landing the fix in `feature_extractor.h`
and documenting the include order in `fex_ctx_vector.cpp`, Wave 3 authors get a
working template to copy.

## Consequences

- **Positive**: `feature_extractor_vector_init` cannot leak the initial storage on
  future early-return paths due to the vector-reserve guard. The `<atomic>` bridge
  fix in `feature_extractor.h` unblocks any future C++ extractor TU that includes
  the header. The stale `test_ansnr_simd` meson stub is removed (fixes a pre-existing
  configuration-time crash on fresh builds post-ansnr-drop).
- **Negative**: `core/test/meson.build` must compile `../src/fex_ctx_vector.cpp`
  (not `.c`) for `test_feature_extractor`; mixed-language `executable()` targets are
  well-supported by meson, and `metadata_handler.cpp` is already compiled the same
  way in this target.
- **Neutral / follow-ups**:
  - Wave 3 candidates: `dict.c`, `opt.c` (require `std::expected` → `cpp_std=c++23`
    project-wide bump, deferred).
  - Project-level `cpp_std=c++23` bump: after Wave 3 landing.
  - `docs/development/cpp23-extractor-pattern.md`: ships with this PR to document
    the `extern "C"` + pre-`<atomic>` include pattern for future extractor authors.

## References

- ADR-0708 (`docs/adr/0708-vmafx-cpp23-internals-pilot.md`) — establishes the
  per-file conversion policy and Wave scheduling.
- Research-0732 (`docs/research/0732-vmafx-cpp23-internals-migration-plan.md`) —
  ROI ranking; `fex_ctx_vector.c` ranked Wave 2 ROI 1.0.
- ADR-0720 (`docs/adr/0720-vmafx-drop-ansnr.md`) — ansnr feature drop; the stale
  `test_ansnr_simd` meson stub removed in this PR is a residual from that change.
- req: "Internal implementation moves `.c` to `.cpp` where C++23 features help."
