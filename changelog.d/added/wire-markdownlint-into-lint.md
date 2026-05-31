## Added

- **`markdownlint-cli2` wired into the lint pipeline** (ADR-0866):
  - `make lint-md` — runs `markdownlint-cli2` against the touched
    markdown delta vs `origin/master`. `make lint` now depends on
    `lint-md`. Override with `MDLINT_SCOPE=all` to lint the full
    corpus (`docs/**/*.md changelog.d/**/*.md README.md CLAUDE.md
    AGENTS.md`).
  - `.pre-commit-config.yaml` — adds the official
    `DavidAnson/markdownlint-cli2` hook with `pass_filenames: true`
    so only staged markdown is linted. No `--fix` (ADR-0864
    documents 7 default rules unsafe to auto-fix).
  - `.github/workflows/lint-and-format.yml` — adds a
    `Markdown lint (markdownlint-cli2)` job that mirrors the
    `clang-tidy` job's PR / push / dispatch delta-collection
    pattern. Uses `actions/setup-node@v4` + `npx --yes
    markdownlint-cli2` on the changed markdown set.

  Touched-file scope means PRs only see warnings for files they
  actually changed — the ~6.2k pre-existing tail from PR #332's
  initial sweep does not gate innocent PRs. Lands after #332
  (which ships the tuned `.markdownlint.json` + initial sweep).
