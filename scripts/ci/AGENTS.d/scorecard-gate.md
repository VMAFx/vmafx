---
paths:
  - scripts/ci/scorecard_gate.py
  - scripts/ci/tests/test_scorecard_*.py
  - .github/workflows/scorecard*.yml
invariant: Complete check sets on exact source bytes; never latest-API results, the merge SHA or a dropped check.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# Exact-source Scorecard reports (ADR-1247)

`scorecard_gate.py` validates complete reviewed check sets and tool identity,
recomputes risk-weighted unrounded score, rejects scanner errors. Keep
all zero and inconclusive states in summaries; only Signed-Releases/-1 with
exact reason `no releases found` unassessed rather than error. PR-local
reports have no upstream commit identity: preserve Git-object byte/mode checks,
extra-input rejection, before/after run-bound receipts. Every followed
symlink component must track; do not permit links through Git metadata or
other mutable inputs even when final file tracked. Preserve literal
symlink targets and legitimate directory chains, with bounded cycle rejection.
Git subprocesses and fixtures must clear inherited GIT_* and caller
global/system configuration.
Never use latest public API results, merge SHA instead of PR head, or omit
check to improve denominator. Workflow/aggregator and source-tamper
controls run through `scorecard-policy-contract` hook and both gate jobs.
Preserve companion ADR-1248 offline repository-policy hook and live master
checker; neither local fixture pass nor aggregate score proves settings.
