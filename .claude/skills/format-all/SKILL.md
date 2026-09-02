---
name: format-all
description: Apply clang-format, black, ruff, and shfmt across the whole repo. Idempotent; safe to run repeatedly.
---

<!-- markdownlint-disable MD013 -->

# /format-all

## Invocation

```text
/format-all [--check]
```

## Steps

1. Verify formatters present:
   - `clang-format --version` (≥ 18 — uses our `.clang-format`)
   - `black --version`
   - `ruff --version`
   - `shfmt -version`
2. Run in parallel (each scoped to its file types):
   - `clang-format -i $(git ls-files '*.c' '*.h' '*.cpp' '*.hpp' '*.cu' '*.cuh')`
   - `black python/ ai/ scripts/`
   - `ruff check --fix-only python/ ai/ scripts/`  # import sorting (ADR-1126)
   - `shfmt -w -i 2 -ci $(git ls-files '*.sh')`
3. With `--check`: replace `-i` / `-w` with `--dry-run` / `-d` and exit 1 on any
   diff. This is the CI mode.
4. Print a per-formatter file count: `clang-format: 312 files, black: 47 files, ...`

## Guardrails

- Never formats files under `subprojects/` (vendored upstream code) or
  `core/test/data/`.
- Never reformats Netflix-authored files in a way that diverges from upstream
  style — our `.clang-format` matches upstream's settings.
- Refuses to run if `git status` shows un-staged conflicts.
