<!-- markdownlint-disable MD013 MD025 MD060 -->
# ADR-3557: Queue same-head pull-request runs on merge-gate workflows

- **Status**: Accepted
- **Date**: 2026-11-17
- **Deciders**: Project maintainer
- **Tags**: ci, fork-local

## Context

The `master` ruleset requires one status-check context: `Required Checks Aggregator` (`.github/workflows/required-aggregator.yml`, job `aggregator`, [ADR-0313](0313-ci-required-checks-aggregator.md)). GitHub evaluates branch protection across the latest check run of every check suite on the head commit, so a `cancelled` `Required Checks Aggregator` run in one check suite keeps the pull request `BLOCKED` even when a sibling suite on the same head passes (pull request 3445 rollup: `CANCELLED` at `01:35:32Z` plus `SUCCESS` at `01:48:19Z`, rollup state `FAILURE`).

Since [ADR-3551](3551-aggregator-edited-window.md), `required-aggregator.yml` triggers on `[opened, edited, synchronize, reopened, ready_for_review]` with `group: required-aggregator-${{ github.workflow }}-${{ github.ref == 'refs/heads/master' && github.sha || github.ref }}` and `cancel-in-progress: ${{ github.ref != 'refs/heads/master' }}`. A review bot edits the pull-request body 7-10 seconds after each push (`pull_request` action `edited`), so the `edited` run cancels the running push run of the same head (observed on pull requests 3434, 3445, 3456, 3505, 3510, and 3522 on 2026-11-17). `.github/workflows/rule-enforcement.yml` (`Rules`) has the same defect: its triggers include `edited` with `group: rule-enforcement-${{ github.event.pull_request.number }}` and `cancel-in-progress: true`, while on `edited` the aggregator correlates `editedRerunWorkflows` by the `"[<pull_request.updated_at>] "` `display_title` prefix and fails when that `Rules` run's jobs are cancelled (pull request 3456, aggregator run `43996540618` attempt 3: nine `Rules` jobs cancelled, aggregator failed); re-running one twin cancels the other.

## Decision

Keep the existing concurrency `group:` keys in `.github/workflows/required-aggregator.yml` and `.github/workflows/rule-enforcement.yml`, and enable `cancel-in-progress` only for `synchronize` pull-request events:

- In `.github/workflows/required-aggregator.yml`, set `cancel-in-progress: ${{ github.event_name == 'pull_request' && github.event.action == 'synchronize' }}`, leaving triggers, `permissions:`, `group:`, and the embedded `script:` block unchanged.
- In `.github/workflows/rule-enforcement.yml`, set `cancel-in-progress: ${{ github.event.action == 'synchronize' }}`, leaving triggers, `permissions:`, and `group:` unchanged.
- Under GitHub Actions concurrency (at most one running and one pending run per group), a `synchronize` push to a new head still cancels the stale running run, while same-head `edited`, `reopened`, and `ready_for_review` runs wait as pending. When a newer pending run replaces an older pending one, the replaced pending run ends `cancelled` with zero jobs and zero check runs (`release-please.yml` runs `37229262831` and `37230781106`), adding no cancelled check run to the required context. Leave the 24 other pull-request workflows unchanged: none reports a required context directly on branch protection, and the aggregator's `newestByName` selection picks their surviving newer twin.
- Extend `scripts/ci/tests/test_master_concurrency_contract.py` (`pr_event`, `gate_workflows`, `same_head_findings`, `skip_findings`) so the gate workflow set (`required-aggregator.yml` plus basenames in `editedRerunWorkflows`, failing on a missing or empty array) forbids same-head cancellation on every non-`synchronize` (`!= 'opened'`, `!= 'closed'`) `pull_request` type at workflow and job level, verifies `synchronize` across distinct SHAs still shares a group with `cancel-in-progress` true, and forbids any `if:` on `jobs.aggregator` or step-level `if:` in that job mentioning `github.event.action` or `edited`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Option a: keep existing `group:` and set `cancel-in-progress` true only on `pull_request` `synchronize` | Preserves master SHA isolation and new-head superseding; same-head `edited`, `reopened`, and `ready_for_review` runs wait as pending and collapse to the newest with zero check runs on replaced pending suites | Same-head `edited` twin waits for the running push run to finish; re-running a `synchronize` run while an `edited` twin runs cancels the twin | Chosen; stops same-head twin cancellation without extra API calls or schema extensions |
| Option b: head-SHA concurrency group with `cancel-in-progress: true` | Separates distinct commits into distinct concurrency groups | A same-head `edited`, `reopened`, or `ready_for_review` event shares the head SHA and still cancels the same-head twin, while a push to a new head no longer cancels the older head's run | Rejected: still cancels the same-head twin |
| Option c: `concurrency.queue: max` | Declares queueing explicitly in workflow YAML | The pinned `actionlint` v1.7.12 in `.pre-commit-config.yaml` refuses it (`unexpected key "queue" for "concurrency" section`), and it cannot combine with `cancel-in-progress` | Rejected: `actionlint` 1.7.12 refuses `queue`, and it cannot combine with `cancel-in-progress` |
| Option d: job-level `if:` skipping `jobs.aggregator` on `edited` | Starts no second aggregator job on pull-request edits | Branch protection counts a `skipped` required status check as passing, which masks a failed or pending aggregator run on the same head | Rejected: `skipped` counts as passing and masks a failed run |
| Option e: per-head-SHA group without cancel plus an in-script supersede check | Avoids Actions concurrency cancellation between same-head suites | A `synchronize` push to a new head no longer cancels the stale run natively (holding runner slots until script polling notices) and adds extra GitHub API calls on every poll | Rejected: new heads no longer cancel stale runs natively, and polling adds extra API calls |

## Consequences

- **Positive**: same-head `edited`, `reopened`, and `ready_for_review` events (including automated review-bot body edits seconds after a push) no longer cancel a running `Required Checks Aggregator` or `Rules` run of the same head; repeated pending edits collapse to the newest pending run with zero check runs on replaced pending suites; `synchronize` pushes to a new head still cancel superseded runs immediately; master push SHA isolation ([ADR-1673](1673-master-runs-not-cancelled-by-concurrency.md)) holds unchanged.
- **Negative**: a same-head `edited` run of `required-aggregator.yml` or `rule-enforcement.yml` waits in `pending` until the running push run finishes. Residual risk: manually re-running a same-head `synchronize` run while its `edited` twin is running evaluates `github.event.action == 'synchronize'` to true and cancels the running `edited` twin (`docs/development/ci.md` instructs operators to re-run a cancelled aggregator only when no aggregator run of the same head is in progress).
- **Neutral / follow-ups**: open question (present since [ADR-3551](3551-aggregator-edited-window.md)): because GitHub counts the latest check run of every check suite on the head commit, an earlier failed `Required Checks Aggregator` suite on the same head stays red on the commit rollup even after a passing `edited` aggregator run until the failed suite itself is re-run or a new commit is pushed. `scripts/ci/tests/test_master_concurrency_contract.py` enforces same-head queueing, new-head superseding, `editedRerunWorkflows` discovery, and the no-skip rule across repository and synthetic workflows.

## References

- `T-CI-AGGREGATOR-SAME-HEAD-CANCEL-2026-11-17` in [docs/state.md](../state.md).
- [ADR-0313: Single required-checks aggregator](0313-ci-required-checks-aggregator.md).
- [ADR-1673: A push to master never cancels the runs of an earlier master commit](1673-master-runs-not-cancelled-by-concurrency.md).
- [ADR-3551: Anchor edited pull-request aggregator runs at ready-for-review](3551-aggregator-edited-window.md).
- [CI overview: Master push runs](../development/ci.md#master-push-runs).
- `scripts/ci/tests/test_master_concurrency_contract.py`: `gate_workflows`, `same_head_findings`, `skip_findings`, repository checks, and positive/negative/boundary synthetic workflow cases.
- Source: decision `vmafx-aggregator-same-head-queue` (2026-11-17, merge-gate same-head concurrency, paraphrased): restrict `cancel-in-progress` to `synchronize` pull-request events in `required-aggregator.yml` and `rule-enforcement.yml` so same-head `edited`, `reopened`, and `ready_for_review` runs queue as pending instead of cancelling the active run.
