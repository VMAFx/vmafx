<!-- markdownlint-disable MD060 -->
# ADR-0733: C++23 Wave 4 — output writers (XML, JSON, CSV, subtitle)

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `build`, `c++`, `cpp23`, `refactor`, `internals`, `fork-local`, `vmafx-rebrand`

## Context

ADR-0708 established the per-file C++23 migration recipe for libvmaf internals.
Waves 1–3 covered `metadata_handler.c`, `mem.c`, `opt.c`, `fex_ctx_vector.c`,
`log.c`, `dict.c`, `feature/psnr_tools.c`, `feature/luminance_tools.c`,
`feature/mkdirp.c`, `feature/feature_name.c`, `feature/picture_copy.c`, and
`model.c` (in-flight PRs #41, #43, #44, #45, #48, #51, #54).

Wave 4 targets `core/src/output.c` — the single file containing all four
format writers (XML, JSON, CSV, MicroDVD subtitle). At 467 lines it is the
largest non-feature C source in the critical path of every CLI invocation. The
file already embodied sound logic (ADR-0606 bounds fixes, ADR-0602 NULL guards,
CERT-C `ferror` pattern), making it an ideal candidate for a mechanical idiom
upgrade with zero output-byte change.

The `LocaleGuard` RAII class eliminates the explicit push/pop pair that would
otherwise require careful placement on every exit path. `std::string_view`
removes the implicit `strlen` penalty on repeated `fmt_or_default` returns and
makes the score-format contract explicit. `[[nodiscard]]` on the four public
entry points turns silently-discarded error codes into a compile-time
diagnostic.

## Decision

Convert `core/src/output.c` to `core/src/output.cpp` using the Wave 1/3
isolated `static_library` + `override_options=['cpp_std=c++23']` pattern from
ADR-0708. The public C ABI (`vmaf_write_output_xml`, `_json`, `_csv`, `_sub`)
is preserved unchanged via `extern "C"` guards in `output.h` and a wrapping
`extern "C" { }` block in `output.cpp` around the internal C headers
(`feature/alias.h`, `feature/feature_collector.h`, `thread_locale.h`) that
lack their own `extern "C"` guards.

Also remove the stale `test_ansnr_simd` executable and `test()` registration
from `core/test/meson.build`, which referenced the deleted `test_ansnr_simd.c`
source file that was removed when the legacy ansnr feature was dropped in
ADR-0720 (commit `70ed8b3ce3`). This was a pre-existing issue that blocked
`meson setup` in this worktree.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Per-format file split (output_xml.cpp, output_json.cpp, etc.) | Finer granularity; each format independently reviewable | The four writers share `max_capacity`, `count_written_at`, `fmt_or_default`, and `pool_method_name` — splitting would require a private header or duplicating helpers. The existing single-file layout was deliberate. | Not chosen: extra complexity for no correctness benefit |
| `std::ofstream` instead of C `FILE*` | Pure C++ I/O | The public API (`vmaf_write_output_xml(..., FILE *outfile, ...)`) passes `FILE*` from the caller; changing to `std::ofstream` would break the C ABI | Not chosen: ABI-breaking |
| `std::format` instead of `fprintf` + `sf.data()` | Type-safe formatting; no format-string mismatch | `std::format` cannot consume a runtime format string from the caller (the `score_format` parameter); and `std::format` float formatting differs subtly from `printf("%.6f", ...)` at the last digit for some edge values, risking golden-gate drift | Not chosen: ABI + numerical parity risk |
| Global `cpp_std=c++23` bump now (instead of isolated lib) | Simpler meson.build | PR #48 (dict + global cpp_std bump) is in the merge train; changing globally here would conflict | Not chosen: merge-train discipline |

## Consequences

- **Positive**: `LocaleGuard` makes locale cleanup unconditional on all future
  exit paths. `[[nodiscard]]` on all four public functions turns silent
  error-code discard into a compile diagnostic. `std::string_view` eliminates
  implicit `strlen` on the score-format string per write call.
- **Negative**: C++ compiler required for this TU (already true for `svm.cpp`,
  `metadata_handler.cpp`; no new toolchain requirement).
- **Neutral**: The stale `test_ansnr_simd` meson.build entries are removed as a
  fix-preexisting-bugs-too action per CLAUDE.md §12 r12. No new tests needed
  for the Wave 4 conversion itself — `test_output` and the existing 49-test
  fast suite cover the output paths; output bytes are verified bit-equivalent
  via the smoke test in the PR description.

## References

- ADR-0708 (cpp23 pilot recipe + Wave 1 isolated lib pattern)
- ADR-0720 (legacy ansnr feature drop — source of the stale test_ansnr_simd ref)
- ADR-0606 (bounds-check correctness in output.c — preserved verbatim)
- ADR-0602 (NULL guard additions — preserved verbatim)
- req: "Convert cpp23 Wave 4 candidates: pick 2-3 small `core/src/output_*.c`
  writer files and convert them to `.cpp` with real C++23 idioms, following
  ADR-0708's playbook."
