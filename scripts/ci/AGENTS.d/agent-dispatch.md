---
paths:
  - scripts/ci/agent-eligibility-precheck.py
  - scripts/lib/backlog_tracker.py
  - scripts/ci/tests/test_agent_eligibility_precheck.py
invariant: Exit codes 0 / 1 / 2 and `::error title=...::` stderr format are dispatcher contract.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# Agent dispatch precheck

## Workflow coupling

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `agent-eligibility-precheck.py` | (no workflow lane today; called manually from `.claude/workflows/*.md` per [ADR-0355](../../../docs/adr/0355-symphony-agent-dispatch-infra.md)) | Loads `scripts/lib/backlog_tracker.py`; two files move together. exit-code contract (0 = eligible, 1 = block, 2 = bad CLI usage) and `::error title=...::...` stderr format are **the** dispatcher contract. Missing rows, unreadable task files, and unavailable or failed GitHub queries block dispatch unless operator selected corresponding explicit `--skip-*` flag. Never change contract without updating `.claude/workflows/_template.md` and `docs/development/agent-dispatch.md` in same PR. |
