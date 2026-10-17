## CodeQL analyses every language on every event (2026-10-13)

- `codeql-cpp`, `codeql-python` and `codeql-actions` in
  `.github/workflows/security-scans.yml` run `init` and `analyze` without a
  step-level `if:` and without the impact planner (ADR-3294).
- The `python_lint` selector is gone from `.github/ci-impact.json`.
- **On sync**: fork-only files; no upstream counterpart. Do not bring back an
  impact gate on a CodeQL job: a skipped category makes GitHub's `CodeQL` check
  neutral, which the aggregator blocks (ADR-3293).
  `scripts/ci/test_security_workflow_contract.py` fails on such a gate.
