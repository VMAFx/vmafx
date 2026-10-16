- **Advisory exceptions in `osv-scanner.toml` expire**: every `[[IgnoredVulns]]` entry
  now needs an `ignoreUntil` date, and `scripts/ci/lint_exceptions.py check` (pre-commit
  hook `check-lint-exceptions`) fails on a missing or expired date. GHSA-84m7-p3x7-pcfv
  (`mcp` 1.29.0, reached only through `semgrep` in the tooling-test lock) is declared until
  2026-10-26; GO-2026-5932 until 2027-02-13.
