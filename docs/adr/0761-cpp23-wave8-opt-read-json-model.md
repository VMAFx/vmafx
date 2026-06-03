# ADR-0761: C++23 Wave 8 — opt.cpp activation + read_json_model.cpp

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `build`, `c++`, `cpp23`, `refactor`, `internals`, `fork-local`

## Context

The ADR-0708 playbook established the pattern for incrementally converting
`core/src/*.c` files to C++23: rename to `.cpp`, compile as an isolated
`static_library` with `override_options: ['cpp_std=c++23']`, preserve the
public C ABI via `extern "C"` guards in headers, and apply real C++23 idioms.

Wave 8 addresses two files:

- **`opt.cpp` (activation)**: `opt.cpp` was created in Wave 1 (commit
  7a62d2f46e) with C++23 idioms (`std::optional`, `std::string_view`,
  `[[nodiscard]]`), but the meson.build was never updated to activate it —
  the build still compiled `opt.c`. Wave 8 completes the Wave 1 activation:
  the isolated `opt_cpp23_lib` static library is added and `opt.c` is removed
  from `libvmaf_sources`. `extern "C"` guards are added to `opt.h`.

- **`read_json_model.c` (639 LOC, new conversion)**: The JSON model parser
  depends only on `pdjson` (already `extern "C"` guarded), `svm.h` (already
  guarded), `thread_locale.h` (already guarded), and three internal headers
  (`log.h`, `model.h`, `read_json_model.h`) that required `extern "C"` guards
  to be added. Conservative C++23 idioms applied: `nullptr`, `static_cast<>`,
  `[[nodiscard]]` on the four public entry points, C++ `<c*>` headers, removal
  of the `goto fail` pattern (replaced with `if/else` scoping to avoid
  jump-past-initialization errors in C++).

  `predict.c` was considered but skipped: it calls into `feature_extractor.h`,
  `feature_collector.h`, `feature_name.h`, and `alias.h`, none of which have
  `extern "C"` guards. Adding those guards would touch a larger footprint
  than a safe Wave 8 conversion should.

Both new isolated static libs are linked into `libvmaf.so` via
`extract_all_objects()`.

## Decision

Activate `opt.cpp` as `opt_cpp23_lib` and convert `read_json_model.c` to
`read_json_model.cpp` as `read_json_model_cpp23_lib`. Both compile under
`cpp_std=c++23` in isolated static libraries. Test executables that previously
included these files as direct sources are updated to pull in objects from the
respective static libs via `wave8_cpp23_objects`. The public C ABI is
preserved unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Convert `predict.c` instead of `read_json_model.c` | Directly exercises the score-prediction path | Requires `extern "C"` guards on `feature_extractor.h`, `feature_collector.h`, `feature_name.h`, `alias.h` — a larger footprint; the task brief's "hidden complexity" exclusion applies | Deferred to a later wave that specifically handles the feature-layer headers |
| Bump project-wide `cpp_std` to `c++23` | Eliminates per-file isolation overhead | Risk of compile failures in unreviewed C TUs that mix C and C++ token rules; contra ADR-0708's incremental isolation policy | ADR-0708 established the per-file pattern |
| Keep opt.c as primary, delete opt.cpp | Zero new complexity | Discards working C++23 code already in tree; adds technical debt | The `.cpp` is already correct; activation is the right move |

## Consequences

- **Positive**: `opt.cpp` is finally active — the C++23 `std::optional`
  helpers replace the old C `strtol`/`strtod` wrappers. `read_json_model.cpp`
  gains `[[nodiscard]]` on all four entry points and cleaner error propagation
  without `goto`. `extern "C"` guards added to `log.h`, `model.h`,
  `read_json_model.h`, and `opt.h` make these headers safe for future C++
  consumers.
- **Negative**: Minor build-graph overhead (two new isolated static lib
  targets). Test binaries that previously compiled opt.c/read_json_model.c
  directly now pull in objects from the static libs — a slightly different
  link order, but semantically identical.
- **Neutral / follow-ups**: `predict.c` remains unconverted; its conversion
  requires extern-C guards on the feature-layer headers (blocked separately).

## References

- [ADR-0708](0708-vmafx-cpp23-internals-pilot.md) — original C++20/23
  migration playbook.
- [ADR-0721](0721-cpp23-pilot-opt.md) — Wave 1: `opt.cpp` created (not yet
  activated). This ADR closes that gap.
- Waves 1–7: prior conversion history.
- Per user direction: Wave 8 bundle of 2 files, conservative C++23 idioms,
  isolated static_library per TU pattern.
