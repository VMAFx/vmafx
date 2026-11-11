---
paths:
  - scripts/ci/check_bandit_new.py
  - scripts/ci/check_flawfinder_new.py
  - scripts/ci/tests/test_check_bandit_new.py
  - scripts/ci/tests/test_check_flawfinder_new.py
  - scripts/lib/new_scanner_findings.py
  - bandit.yml
  - .pre-commit-config.yaml
invariant: New Bandit and flawfinder findings fail locally; base findings survive moves and renames.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# New scanner findings

Codacy: Bandit on Python; flawfinder on C/C++.
Local: pre-commit and pre-push checks; changed files only.
Base: merge base with `origin/master`; old rule plus stripped source line passes after move or rename.
Bandit tests: `bandit.yml`; Codacy API enabled pattern IDs read 2026-11-11.
Refresh: compare Codacy enabled Bandit IDs; update `bandit.yml`; run planted recall test.
Process calls: `scripts.lib.safe_subprocess.run`; `# noqa: S603` leaves Bandit finding active.
Failure: new finding exits 1; missing scanner, git failure, bad JSON, scan error exits 2.
