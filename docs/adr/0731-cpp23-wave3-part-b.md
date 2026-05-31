<!-- markdownlint-disable MD060 -->
# ADR-0731: C++23 Wave 3 Part B — psnr_tools, luminance_tools, mkdirp

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `build`, `cpp23`, `modernization`

## Context

ADR-0708 established the C++23 migration playbook: one isolated `static_library`
per converted TU, `override_options: ['cpp_std=c++23']` to contain the dialect
change until PR #48 (project-wide `cpp_std=c++23`) merges, and a companion
`extern "C"` guard in each header so C callers require no changes.

Wave 1 (PRs #41–#45) and Wave 2 (PR #48 meson + dict.c) addressed core runtime
files. Wave 3 part A (parallel agent) targets feature extractor infrastructure.
Wave 3 part B (this ADR) converts the three smallest utility TUs in
`core/src/feature/` that have no SIMD or GPU twins and therefore need no
parallel backend conversion:

- `psnr_tools.c` (55 LOC) — format-to-PSNR-constant mapping
- `luminance_tools.c` (113 LOC) — BT.1886 / PQ EOTF + luma range helpers
- `mkdirp.c` (96 LOC) — MIT-licensed recursive mkdir

The `core/test/meson.build` also contained a stale reference to
`test_ansnr_simd.c` left over from the ansnr drop (commit `70ed8b3`); the
orphaned entries are removed in this same PR since Wave 3 part B is the first
PR to reconfigure the test build and thus the first to surface the breakage.

## Decision

We will convert `psnr_tools.c`, `luminance_tools.c`, and `mkdirp.c` to `.cpp`
with C++23 idioms, following the ADR-0708 isolated-static-library pattern. Each
file is compiled as a separate `static_library` with
`override_options: ['cpp_std=c++23']`, linked into `libvmaf` via
`extract_all_objects`, and added to every test `objects:` list that previously
included `libvmaf_feature_static_lib`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Wait for PR #48 (global cpp_std=c++23) | No per-TU override boilerplate | Blocks progress; PR #48 may be delayed by merge-train ordering | Rejected — Wave 3 proceeds independently; override_options is harmless after #48 merges |
| Convert only one file (psnr_tools) | Smaller diff | Misses the other two utilities that are equally suitable; wave cadence slows | Rejected — all three are similarly sized, have no SIMD/GPU twins, and are safe to bundle |
| Inline the converted objects directly into libvmaf_feature_static_lib | Fewer static libs | Spreads C++23 dialect to all C TUs in libvmaf_feature before #48 merges | Rejected — isolation wrapper is the ADR-0708 invariant |

## Consequences

- **Positive**: Three more TUs gain `std::string_view`, `std::optional`,
  `std::clamp`, `constexpr` constants, `[[nodiscard]]`, RAII cleanup, and
  `nullptr`. The `goto fail` pattern in `mkdirp.c` is replaced by RAII
  `std::string`. The format-dispatch `strcmp` chain in `psnr_tools.c` becomes a
  compile-time `constexpr` table. Stale `test_ansnr_simd` entries are purged.
- **Negative**: Three additional isolated static libs add minor meson complexity.
  Every test `objects:` block that links `libvmaf_feature_static_lib` must also
  include `feature_cpp23_objects` (defined as a helper list in
  `core/test/meson.build`).
- **Neutral / follow-ups**: After PR #48 merges, the `override_options` can be
  dropped from each lib, but the isolation wrappers remain for build hygiene
  per the ADR-0708 precedent.

## References

- [ADR-0708](0708-cpp23-migration-playbook.md) — C++23 migration playbook
- Related PR: cpp23 Wave 3 part B (this PR)
- Wave 3 part A: parallel agent working `core/src/feature/feature_collector.c`,
  `feature_name.c`, `picture_copy.c`
- req: "Convert cpp23 Wave 3 part B candidates: pick 2–3 small
  `core/src/feature/*.c` files (utilities, NOT extractors with SIMD/GPU twins)"
