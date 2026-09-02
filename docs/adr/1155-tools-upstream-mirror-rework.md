<!-- markdownlint-disable MD013 MD060 -->
# ADR-1155: Rework CLI and Tools Translation Units to Fork Lint Standards

- **Status**: Accepted
- **Date**: 2026-09-02
- **Deciders**: lusoris, kilian, antigravity
- **Tags**: tools, cli, clang-tidy, lint, dead-twin, portability, c23, cpp23

## Context

The `core/tools/` subtree contains the standalone CLI binaries (`vmaf`,
`vmaf_bench`) and their supporting input and argument parsers (`cli_parse.cpp`,
`y4m_input.c`, `cli_parse.h`). As of 2026-08-31, these files accumulated 348
warnings under the whole-tree `clang-tidy` ratchet profile introduced in ADR-1142:

- `core/tools/cli_parse.cpp`: 107 warnings
- `core/tools/cli_parse.c`: 84 warnings (un-deleted dead twin)
- `core/tools/vmaf.cpp`: 69 warnings
- `core/tools/y4m_input.c`: 66 warnings
- `core/tools/vmaf_bench.c`: 22 warnings

On 2026-08-31, maintainer direction affirmed that upstream code not yet reworked
to fork engineering standards must be brought to zero warnings, preserving only
the Netflix golden-score assertions and runtime CLI behavior as invariants.

Furthermore, `core/tools/cli_parse.c` was identified as a suspected dead twin
of `core/tools/cli_parse.cpp`. Under ADR-1153 precedent, dead twins cannot be
deleted without a function-by-function audit demonstrating that all unique
behaviors or assertions are preserved on the live side.

Finally, `y4m_input.c` and `vmaf_bench.c` are C translation units. Under ADR-1138,
C TUs must retain `NULL` rather than C23 `nullptr` to avoid breaking MSVC's
`/std:clatest` C compiler on Windows CI legs.

## Decision

1. **Rework CLI and Tools in Place to Fork Standards**:
   - `core/tools/vmaf.cpp`: Reworked to modern C++23. Implemented RAII cleanup
     guards for file descriptors, picture allocations, and model contexts;
     decomposed monolithic routines into modular helper functions; eliminated
     C-style casts in favor of explicit `static_cast`; ensured clean error
     propagation. Reported warnings reduced from 69 to 0.
   - `core/tools/cli_parse.cpp`: Modernized to C++23. Uses `std::string_view`
     for exact string comparisons; decomposes argument parsing into typed
     helper functions; adds `[[nodiscard]]` and `[[noreturn]]` annotations.
     Reported warnings reduced from 107 to 0.
   - `core/tools/cli_parse.h`: Modernized header guard to `VMAF_CLI_PARSE_H`
     (removing leading double underscore reserved by standard). Verified
     self-contained under both C23 and C++23 compilers.

2. **C Translation Unit Portability (ADR-1138 Contract)**:
   - `core/tools/y4m_input.c` and `core/tools/vmaf_bench.c`: Retain `NULL` as the
     null pointer constant. Bounded with file-scoped
     `/* NOLINTBEGIN(modernize-use-nullptr) */` / `/* NOLINTEND(modernize-use-nullptr) */`
     blocks explicitly citing ADR-1138 to ensure MSVC `/std:clatest` builds on
     Windows CI continue to compile without error.
   - Fixed all remaining arithmetic, linkage, and cast warnings. Reported
     warnings reduced to 0 (`y4m_input.c`: 66 -> 0; `vmaf_bench.c`: 22 -> 0).

3. **Dead-Twin Resolution for `cli_parse.c` (ADR-1153 Precedent)**:
   - A function-by-function audit compared `cli_parse.c` against `cli_parse.cpp`:
     `resolve_precision_fmt`, `long_opts`, `usage`, `error`, `parse_unsigned`,
     `parse_bitdepth`, `parse_pix_fmt`, `strsep`, `parse_model_config`,
     `parse_feature_config`, `aom_ctc_*`, `nflx_ctc_*`, `cli_parse`, `cli_free`.
   - Result: `cli_parse.c` carried zero unique behaviors, functions, or
     assertions. In fact, `cli_parse.cpp` contained strict bounds checks
     (`--width > 0` and `--height > 0`) that were missing in `cli_parse.c`.
   - `cli_parse.c` was an orphan remaining from the ADR-0809 C++23 rewrite
     that had only remained referenced by `test_cli_parse`,
     `test_cli_parse_long_only_args`, and `fuzz_cli_parse`.
   - `cli_parse.c` is deleted. `core/test/meson.build` and
     `core/test/fuzz/meson.build` are rewired to compile `cli_parse.cpp`.
     `scripts/ci/twin-drift-check.sh` passes cleanly with zero unlisted dead sides.

4. **Strict Behavioral Preservation (CLI Parity Matrix)**:
   - Behavior was validated against a pristine build of `origin/master` across
     a comprehensive CLI test matrix. Both binaries produce byte-identical
     stdout, stderr, and exit codes across:
     `--version`, `--help`, Y4M input, missing-file error, wrong-argument error,
     `vmaf_bench`, and scoring across `--json`, `--xml`, `--csv`,
     `--precision=max`, `--feature psnr`, two `--model` versions, `--threads 4`,
     and `--subsample 2`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Blanket `// NOLINT` across all files | Quickest path to passing CI | Leaves technical debt; masks real defects; violates ADR-0141 and ADR-1142 | Violates core project engineering principles and maintainer direction |
| Leave upstream mirror untouched | Zero merge conflicts during upstream sync | Violates whole-tree clang-tidy ratchet; leaves 348 warnings | Directly contradicts 2026-08-31 maintainer mandate |
| Rewrite CLI from scratch | Complete architectural redesign | High regression risk; breaks backwards compatibility with Netflix scripts | High risk with no user benefit |
| Retain `cli_parse.c` as a test-only twin | No changes needed to test build definitions | Duplicates 1,100+ lines; subject to silent twin drift | Precedent set by ADR-1153 requires eliminating verified dead twin sides |

## Consequences

- **Positive**: 348 clang-tidy warnings eliminated; all CLI translation units
  achieve 0 warnings; dead twin deleted; clean twin-drift check.
- **Negative**: Upstream syncs touching `core/tools/` will require conflict
  resolution (documented in `docs/rebase-notes.md`).
- **Neutral / follow-ups**: Baseline tightened in `scripts/ci/tidy-baseline-cpu.json`.

## References

- [ADR-0141](0141-touched-file-cleanup-rule.md): Touched file cleanup rule
- [ADR-0809](0809-vmaf-cli-cpp23-port.md): CLI C++23 migration
- [ADR-1138](1138-c-translation-units-keep-null.md): C translation units keep NULL
- [ADR-1142](1142-whole-tree-clang-tidy-ratchet.md): Whole-tree clang-tidy ratchet
- [ADR-1153](1153-twin-dead-sides-resolution.md): Twin dead sides resolution
- Maintainer direction (2026-08-31): Upstream code not reworked to fork standards must be reworked, with Netflix goldens as the only invariant.
