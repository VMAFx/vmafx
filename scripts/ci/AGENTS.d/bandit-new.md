---
paths:
  - scripts/ci/check_bandit_new.py
  - scripts/ci/check_flawfinder_new.py
  - scripts/ci/tests/test_check_bandit_new.py
  - scripts/ci/tests/test_check_flawfinder_new.py
  - scripts/ci/tests/scanner_test_support.py
  - scripts/lib/new_scanner_findings.py
  - bandit.yml
  - .pre-commit-config.yaml
invariant: Outgoing blobs versus merge base; config changes scan all Python; Git-detected renames retain findings.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# New scanner findings

Codacy: Bandit on Python; flawfinder on C/C++.
Local: pre-commit -> working tree; pre-push -> `PRE_COMMIT_TO_REF` blobs.
Base: merge base with outgoing ref or HEAD; `origin/HEAD` symbolic target, else `origin/master`; `--base-branch` override.
Identity: old rule plus stripped source line; multiplicity preserved; moved lines and Git-detected renames retain findings.
Low-similarity rename: no pairing -> retained findings reported as new; fail closed.
Bandit list: root `bandit.yml`; Codacy reads analysed branch after configuration-file toggle enabled.
Comparison: current config versus merge-base config; absent base config -> current config.
Config diff: every tracked Python source scanned; unchanged source still compared under both configs.
Hook: `require_serial: true`; parallel partitions each repeat config-diff full scan.
Selection: identify Python + pyi tags; shebang files included; PEP 263 decoding; literal Git paths.
Shared revision selection: Bandit + flawfinder; outgoing files independent of working tree.
Outgoing + local scans: symlinks + gitlinks skipped; base links count absent.
IDs: 56 enabled defaults from codacy-bandit `docs/patterns.json`, version 1.9.4; checked 2026-11-11.
Research: `docs/research/3530-bandit-codacy-pattern-list.md`; decision: ADR-3530.
Fixtures: shared hook-variable isolation; caller `PRE_COMMIT_*` removed until test cleanup; explicit per-test refs retained.
Absent branch config -> Codacy UI patterns; local missing current config -> error.
Refresh: compare Codacy enabled Bandit IDs; edit `bandit.yml`; Codacy follows; run planted recall test.
Process calls: `scripts.lib.safe_subprocess.run`; `# noqa: S603` leaves Bandit finding active.
Failure: new finding exits 1; missing scanner, git failure, bad JSON, scan error exits 2.
