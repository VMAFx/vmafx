---
name: lint-all
description: Run clang-tidy, cppcheck, include-what-you-use, ruff, and semgrep in parallel; produce a single merged report.
---
# /lint-all

## Invocation

```text
/lint-all [--fix] [--changed-only] [--severity=warning|error]
```

## Steps

1. Ensure `build/compile_commands.json` exists (missing -> run
   `meson setup build`; linters need it).
2. Target file set:
   - Default: all C/C++/Python/shell sources tracked by git.
   - `--changed-only`: `git diff --name-only origin/master...HEAD`.
3. Run linters in parallel; write JSON to `build/lint/<tool>.json`:
   - `clang-tidy -p build --config-file=.clang-tidy <files>`
   - `cppcheck --enable=all --suppressions-list=.cppcheck-suppressions.txt
     --project=build/compile_commands.json`
   - `include-what-you-use` (via `iwyu_tool.py -p build`)
   - `ruff check python/ ai/ scripts/`
   - `semgrep --config=.semgrep.yml`
4. Merge into report (`build/lint/report.md`), group by file:line, sort by
   severity. Entry: `[tool] severity message (rule-id)`.
5. With `--fix`: apply safe auto-fixes via `clang-tidy --fix-errors`,
   `ruff --fix`. Never auto-fix cppcheck / semgrep findings (too risky).
6. No findings at or above `--severity` (default: error) -> exit 0. Else exit 1.

## Guardrails

- Honor `.clang-tidy` `HeaderFilterRegex`; never lint vendored headers.
- Skip files under `subprojects/`, `build/`, `core/test/data/`.
- semgrep runs offline: no rule fetch over network in CI.
