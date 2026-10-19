## Scorecard gate trusts v5.6.0 (2026-10-15)

- `scripts/ci/scorecard_gate.py`: `VERSION`, `TOOL_COMMIT` and
  `ACTION_COMMIT` name Scorecard v5.6.0 and `ossf/scorecard-action` v2.5.0.
  Fork-only, nothing upstream to merge.
- **On sync or a later bump**: the `uses: ossf/scorecard-action@` lines of
  `scorecard.yml` and `security-scans.yml` move in the same pull request as
  `ACTION_COMMIT`; `test_every_scan_uses_the_action_the_gate_reviewed` fails
  otherwise. Compare `docs/checks/internal/checks.yaml` of both tool commits
  before keeping `WEIGHTS`.
