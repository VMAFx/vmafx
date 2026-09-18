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
3. `--check` mode: swap `-i` / `-w` for `--dry-run` / `-d`; exit 1 on any diff.
   CI mode.
4. Print per-formatter file count: `clang-format: 312 files, black: 47 files, ...`

## Guardrails

- Never format files under `subprojects/` (vendored upstream code) or
  `core/test/data/`.
- Never diverge Netflix-authored files from upstream style —
  `.clang-format` matches upstream settings.
- Refuse to run if `git status` shows un-staged conflicts.
