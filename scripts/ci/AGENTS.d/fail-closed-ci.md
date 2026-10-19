---
paths:
  - scripts/ci/test_fail_closed_ci.py
  - scripts/git-hooks/pre-push-mypy.py
  - scripts/git-hooks/test-pre-push-mypy.py
  - requirements/locks/mypy.txt
invariant: Required Python Lint = hash-locked mypy via merge-base hook, full history, no exit masking; test stays wired.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# Fail-closed CI and required Python Lint

## Required Python Lint reuses the merge-base gate (ADR-1310)

`.github/workflows/lint-and-format.yml` must install
`requirements/locks/mypy.txt` with `--require-hashes`, fetch full history, and
invoke `scripts/git-hooks/pre-push-mypy.py` without `continue-on-error` or
shell success tail. Pull requests use `origin/master`; master pushes pass
`github.event.before` through `VMAFX_MYPY_BASE_REF` so post-merge job does
not compare HEAD with itself. local hook's default stays `origin/master`.

Keep `test_fail_closed_ci.py` wired to Rule Enforcement and to
`fail-closed-ci-contract` hook, with `lint-and-format.yml` in that hook's file
trigger. Its mutation matrix rejects shallow history, unhashed checker install,
missing push-base authority, advisory exit masking, and raw `mypy ai/ scripts/`
directory discovery. Base-override behavior belongs in
`scripts/git-hooks/test-pre-push-mypy.py`; do not duplicate selection or
fingerprint logic in CI-only script.

## Workflow coupling

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `test_fail_closed_ci.py` | `rule-enforcement.yml` — `Verify fail-closed CI contract`; `.pre-commit-config.yaml` — `fail-closed-ci-contract` | Protects real exit propagation for tox coverage, CPU coverage pytest, nightly benchmarks, advisory Semgrep, sanitizer test discovery, and required Python Lint. mypy contract also pins full history, its hash-locked install, exact push-base authority, and canonical merge-base runner. Diagnostic continuation is valid only when final `if: always()` step reasserts recorded raw outcome. Keep both callers wired. |
