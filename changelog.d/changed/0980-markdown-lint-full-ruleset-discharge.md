- **chore(docs):** Discharge the full 21,775-violation markdown-lint
  tail under the original `.markdownlint.json` ruleset (`default: true`,
  MD013@80, MD024 siblings-only). Strategy: safe-autofix the blank-line /
  whitespace subset, programmatically fix MD040 / MD004, and add per-file
  `<!-- markdownlint-disable ... -->` scoped to each file's actual
  violation profile. `.markdownlint.json` is byte-stable vs `origin/master`
  — the gate's rule set is not narrowed. All 23 pre-commit hooks pass
  tree-wide. Supersedes PR #497 / ADR-0979's narrow-the-gate disposition.
  (ADR-0980)
