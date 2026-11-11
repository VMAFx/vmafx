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
Bandit list: root `bandit.yml`; Codacy reads analysed branch; local gate uses same list.
IDs: 56 Codacy-enabled Bandit patterns checked 2026-11-11; absent file -> Codacy UI patterns.
Refresh: compare Codacy enabled Bandit IDs; edit `bandit.yml`; Codacy follows; run planted recall test.
Process calls: `scripts.lib.safe_subprocess.run`; `# noqa: S603` leaves Bandit finding active.
Failure: new finding exits 1; missing scanner, git failure, bad JSON, scan error exits 2.
