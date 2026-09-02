<!-- markdownlint-disable MD013 MD060 -->
# ADR-1003: Bump project-wide C++ standard from c++11 to c++23

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: lusoris
- **Tags**: build, c++, cpp23, meson, standards, fork-local, vmafx-rebrand

## Context

`core/meson.build` has pinned `cpp_std=c++11` since the initial fork
divergence. ADR-0727 (Wave 2 of the C++ migration) documented the decision to
bump the project-wide default to `c++23` as part of that wave, but the
`core/meson.build` change was never landed — the PR (#48) only delivered
`dict.cpp` and the surrounding isolated-lib objects without updating the
`default_options` entry.

Meanwhile, multiple source files have been converted from `.c` to `.cpp` and
compiled under C++23 via per-target `override_options: ['cpp_std=c++23']`:

- `metadata_handler.cpp` (ADR-0708, override `c++20`)
- `log.cpp`, `opt.cpp`, `fex_ctx_vector.cpp`, `picture_pool.cpp`,
  `gpu_picture_pool.cpp`, `gpu_dispatch_env.cpp`, `read_json_model.cpp`
  (ADR-0708 wave / ADR-0768 / ADR-0858 / ADR-0761)
- `feature_extractor.cpp`, `feature_collector.cpp`, `feature_name.cpp`,
  `cpu.cpp`, `ref.cpp`, `thread_locale.cpp`, `picture_copy.cpp`,
  `luminance_tools.cpp`, `psnr_tools.cpp`, `mkdirp.cpp`, `mem.cpp`,
  `model.cpp`, `output.cpp` (subsequent Wave PRs)

The project default of `c++11` is therefore inconsistent with the files
already compiled at C++23. Any future `.cpp` file added directly to
`libvmaf_sources` without its own `override_options` would silently fall back
to C++11 and lose access to `std::expected`, `std::string_view`, CTAD, and
structured bindings that existing C++ TUs rely on.

The CI toolchain (GCC 13+ / Clang 16+ minimum per ADR-0692 context, dev
machine on GCC 16 / Clang 22) fully supports C++23.

## Decision

Set `cpp_std=c++23` in `core/meson.build` `default_options`. The existing
per-target `override_options: ['cpp_std=c++23']` on the isolated static libs
become redundant but are left in place — cleanup is deferred to a follow-up
sweep PR to keep this PR minimal and reviewable.

Also fix `core/test/meson.build`: `test_feature_collector_coverage` referenced
internal non-static helpers from `feature_collector.cpp` but only linked
against `libvmaf` (which includes the C version `feature_collector.c` where
those helpers are `static`). The binary was link-failing since it was
introduced. Fix: compile `feature_collector.cpp` directly into the test
executable (same pattern as `test_predict`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `c++11` project default and always use per-target `override_options` | No change to default | Every new `.cpp` file must remember the boilerplate or silently compiles at C++11; inconsistent with already-present Wave 1–9 files | Fragile; undocumented footgun for contributors |
| Bump to `c++20` instead of `c++23` | Wider legacy toolchain support | `std::expected` (used in `dict.cpp`) requires C++23; fork already targets C++23 per ADR-0727 decision | ADR-0727 decision already mandated C++23 |
| Bump and immediately remove all per-target `override_options` | Maximally clean build | Large diff; harder to bisect; MSVC token (`vc++latest`) still needed for isolated libs pending MSVC C++23 support validation | Defer cleanup; minimal diff is safer first |

## Consequences

- **Positive**: Any new `.cpp` added directly to `libvmaf_sources` compiles at
  C++23 by default; no boilerplate needed. Build is internally consistent.
  `test_feature_collector_coverage` links correctly (pre-existing broken link
  now fixed).
- **Negative**: Build toolchain floor rises to gcc >= 13 / clang >= 16 for
  C++ (unchanged from the Wave 1 precedent; C++ was already at this floor via
  per-target overrides). External builds on Ubuntu 20.04 LTS (gcc 9) must use
  the container image.
- **Neutral / follow-ups**:
  - Sweep PR to remove now-redundant `override_options: ['cpp_std=c++23']`
    from the isolated static libs (except `metadata_handler_cpp20_lib` which
    uses C++20 intentionally, and the MSVC-path `vc++latest` libs).
  - Update `core/AGENTS.md` guidance to reflect that new `.cpp` files no
    longer need isolated-lib boilerplate for C++23.

## References

- [ADR-0708](0708-vmafx-cpp23-internals-pilot.md) — C++23 pilot wave, established per-target pattern.
- [ADR-0727](0727-cpp23-wave2-bump-and-dict.md) — Wave 2 decision to bump project `cpp_std` to `c++23`; this ADR lands what ADR-0727 decided.
- [ADR-0692](0692-vmafx-c23-bump.md) — Bumped C standard to C23.
- [ADR-0702](0702-vmafx-phase4-language-modernization.md) — Phase 4 language modernization policy.
- req: "core/meson.build pins cpp_std=c++11 but ADR-0708/0692 + the C++23 Wave PRs have already converted multiple files to .cpp with C++23 expectations. The c++11 pin is inconsistent."
