- Restored twelve golden fixtures that the pre-commit whitespace hooks rewrote
  during the ruff bump, which left `pkg/benchmark` and `pkg/tune/auto` failing on
  master: four benchmark CSV goldens lost the CRLF line endings Python's `csv`
  module writes, and eight `python_plans/*.json` plan fixtures lost trailing
  whitespace / end-of-file bytes. Both sets are byte-exact expected output, so
  any rewrite is a test failure by construction.
- The cause is the same root-anchored pattern already fixed for markdownlint:
  `trailing-whitespace`, `end-of-file-fixer` and `mixed-line-ending` all excluded
  `^testdata/`, which matches only the repository-root `testdata/` tree and misses
  fixtures under `pkg/*/testdata/`. Broadened to `(^|/)testdata/`.
  `mixed-line-ending` runs with `--fix=lf`, so it is specifically what destroyed
  the CRLF goldens.
- These hooks only ran across the whole tree because the ruff bump needed a
  `pre-commit run --all-files`; in normal use they are scoped to changed files,
  which is why this had never fired before.
