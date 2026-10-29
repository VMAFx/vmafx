## Renovate schedule and pull-request concurrency (2026-10-28)

- `renovate.json`: `updateNotScheduled: false`, `prConcurrentLimit: 2`,
  `branchConcurrentLimit: 3` (ADR-3384). Upstream has no Renovate
  configuration; nothing to merge.
- `.github/workflows/reuse.yml` carries the ADR-1673 concurrency block.
  `scripts/ci/tests/test_master_concurrency_contract.py` refuses a
  pull-request workflow without a superseding workflow-level group unless
  `UNSUPERSEDED` names it with a reason. **On sync**: a workflow brought in
  from upstream with a pull-request trigger gets the block or an entry.
