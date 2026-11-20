- **Same-head pull-request events queue instead of cancelling active merge-gate
  runs.** `.github/workflows/required-aggregator.yml` and
  `.github/workflows/rule-enforcement.yml` now enable `cancel-in-progress` only
  on `pull_request` `synchronize` events: same-head `edited`, `reopened`, and
  `ready_for_review` aggregator runs wait as pending and repeated pending
  aggregator runs collapse to the newest, while each `edited` `Rules` run gets
  its own concurrency group keyed on `github.run_id` (same-second edits sharing
  an `updated_at` stamp fail closed when an aggregator run lists both `Rules`
  runs; the later edit's own aggregator run queues behind the earlier one and
  re-lists them after its own grace unless a third edit replaces that pending
  run, per ADR-3551 and ADR-3557); a content guard fails superseded required
  `Rules` jobs, enforced by
  `scripts/ci/tests/test_master_concurrency_contract.py` (ADR-3557).
