- The `Pre-Commit (Formatters + Basic Checks)` required check passes
  again. The repository ran two independent import sorters over the same
  files — the standalone `isort` hook and ruff's `I` ruleset — and since
  both auto-fix, `pre-commit run --all-files` never reached a fixed
  point: isort rewrote a file and ruff rewrote it back, so the run
  failed while leaving a net-zero `git diff`. The standalone `isort`
  hook, its `Python Lint` CI step, and the `[tool.isort]` config block
  are retired; ruff's `I` ruleset is now the only import sorter
  (ADR-1126). No source file changes were needed — `master` was already
  ruff-clean.
- Four `MD060/table-column-style` markdownlint errors fixed in
  `pkg/codecadapter/AGENTS.md` and `pkg/tune/AGENTS.md` (table delimiter
  rows written tight, `|---|`, where the surrounding table uses the
  spaced "compact" style).
- The `Python Lint` job is renamed to `Python Lint (Ruff + Black + mypy)`
  to match what it runs; the name is updated in
  `required-aggregator.yml` in the same commit, since the aggregator
  matches required checks by exact job name.
