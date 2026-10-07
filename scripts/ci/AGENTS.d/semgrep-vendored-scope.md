---
paths:
  - scripts/ci/tests/test_semgrep_vendored_scope.py
  - .semgrep.yml
  - .semgrepignore
invariant: banned-call rule must report planted defects in vendored paths; vendored finding is fixed, never excluded.
---
<!-- markdownlint-disable MD013 MD060 -->
# Semgrep covers vendored code

## Workflow coupling

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `tests/test_semgrep_vendored_scope.py` | `.pre-commit-config.yaml` — `test-semgrep-vendored-scope` hook (own `semgrep` venv, like `semgrep-local`); required `Pre-Commit` CI job | Planted-defect recall for `vmaf-no-strcpy-strcat-sprintf` rule: copies real `.semgrep.yml` + `.semgrepignore` into throwaway project, plants banned calls at `core/src/mcp/3rdparty/cJSON/` and `core/src/pdjson.{c,h}`, and requires both scan forms (whole tree = CI, explicit files = hook) to report them; then scans real vendored files. It exists because rule-level `paths.exclude` plus `.semgrepignore` line hid vendored cJSON, so 1.7.19 re-vendor reverted ADR-0683 / ADR-1061 with every gate green. finding in vendored code is answered by fixing code, never by exclusion (ADR-1142). `--no-git-ignore` is deliberate: throwaway project may sit under git-ignored `TMPDIR`. |
