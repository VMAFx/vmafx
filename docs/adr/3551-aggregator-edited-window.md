<!-- markdownlint-disable MD013 MD025 MD060 -->
# ADR-3551: Anchor edited pull-request aggregator runs at ready-for-review

- **Status**: Accepted
- **Date**: 2026-11-15
- **Deciders**: Project maintainer
- **Tags**: ci, fork-local

## Context

`.github/workflows/rule-enforcement.yml` triggers on `[opened, edited, synchronize, reopened, ready_for_review]`, while `.github/workflows/required-aggregator.yml` triggered on `[opened, synchronize, reopened, ready_for_review]`. Editing a pull request's title or body after `Required Checks Aggregator` succeeded re-ran the governance gates (`Deliverables Checklist`, `docs/state.md Gate`, `DCO Sign-off`, and sibling jobs in `rule-enforcement.yml`) without re-running the aggregator (issue 3459). A gate turned red by that edit left the single required status check ([ADR-0313](0313-ci-required-checks-aggregator.md)) green, leaving the pull request `UNSTABLE` instead of `BLOCKED`.

Adding `edited` to `required-aggregator.yml` without adjusting its freshness window fails healthy pull requests. The aggregator computed `minCreatedMs = runCreatedMs - 2 * 60 * 1000` from its own workflow run to drop draft-era GitHub Actions check runs. Because build, lint, and test workflows do not trigger on `edited`, their check runs on the unchanged head SHA predate the edited aggregator run and would be ignored as absent, failing every owed context under [ADR-3293](3293-aggregator-owed-contexts-must-succeed.md).

## Decision

Add `edited` to `on.pull_request.types` in `.github/workflows/required-aggregator.yml` (`[opened, edited, synchronize, reopened, ready_for_review]`), leaving workflow `permissions:` (`pull-requests: read` authorises issue timeline reads) and ref-scoped `concurrency:` (`cancel-in-progress: true` off `master`) unchanged.

Derive the check-run freshness anchor in `windowAnchorMs`:

- When `context.eventName === 'pull_request' && context.payload.action === 'edited'`, read `github.rest.issues.listEventsForTimeline` (`per_page: 100`, capped at 10 pages, wrapped in `withApiBackoff`) and anchor at `created_at` of the latest `ready_for_review` timeline event. When the timeline contains no `ready_for_review` event (the pull request was never a draft), anchor at `context.payload.pull_request.created_at`.
- On every other event, anchor at the aggregator workflow run's own `created_at`, unchanged.
- Compute `minCreatedMs = anchorMs - 2 * 60 * 1000` (`runTimestampMs >= minCreatedMs` counts; `< minCreatedMs` is excluded), log the selected anchor source via `core.info`, and fail closed without judging checks if the timeline call fails or the anchor timestamp cannot be derived. Non-Actions `github-advanced-security` checks retain their any-age rule ([ADR-3539](3539-aggregator-app-checks-outside-run-window.md)).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Option a: trigger on `edited` and anchor at latest `ready_for_review` or `pull_request.created_at` | Re-judges re-run governance gates alongside ready-head build checks; preserves draft-era exclusion; works when an edit cancels an in-progress push aggregator | One extra aggregator run and timeline API query per PR title/body edit | Chosen; satisfies branch protection without re-running build matrices |
| Option b: reuse the previous aggregator verdict and re-check only `rule-enforcement.yml` gates | Avoids reading the issue timeline when a prior completed verdict exists | Ref-scoped `cancel-in-progress` cancels an in-progress aggregator when an edit lands mid-run, leaving no completed verdict to reuse | Rejected: `cancel-in-progress` leaves no verdict to reuse |
| Option c: leave `required-aggregator.yml` triggers as is | Zero extra aggregator runs on PR edits | Title or body edits turning governance gates red leave the required check green (`UNSTABLE`, not `BLOCKED`), allowing forge-button merge | Rejected: forge-button merge on `UNSTABLE` |
| Option d: disable `minCreatedMs` on `edited` runs | No timeline API call required | Admits skipped or stale draft-era GitHub Actions check runs from before the pull request became ready for review | Rejected: re-opens the draft-era masking gap |

## Consequences

- **Positive**: editing a pull request's title or body re-runs `Required Checks Aggregator`; a red `Deliverables Checklist`, `docs/state.md Gate`, or `DCO Sign-off` check turns the required context red (`BLOCKED`), while a valid edit counts earlier ready-head build and test runs and stays green. Draft-era GitHub Actions check runs older than `ready_for_review` minus two minutes remain excluded, and timeline errors or missing timestamps fail closed.
- **Negative**: every pull-request title or body edit, including automated bot body edits, triggers one extra `Required Checks Aggregator` polling run and one paginated timeline API read (`listEventsForTimeline`).
- **Neutral / follow-ups**: push, `workflow_dispatch`, `opened`, `synchronize`, `reopened`, and `ready_for_review` runs keep their existing own-run anchor, 2-minute registration grace, tier exemptions, and [ADR-3539](3539-aggregator-app-checks-outside-run-window.md) external-app handling. `scripts/ci/required_aggregator_harness.py` and `scripts/ci/tests/test_aggregator_edited_window.py` cover edited pass/fail paths, boundary timestamps, draft-era exclusion, multiple `ready_for_review` transitions, pagination, and transient/permanent timeline failures.

## References

- Issue 3459 (`T-CI-AGGREGATOR-EDITED-WINDOW-2026-11-15`).
- [ADR-0313: Single required-checks aggregator](0313-ci-required-checks-aggregator.md).
- [ADR-3293: Every owed required context must succeed](3293-aggregator-owed-contexts-must-succeed.md).
- [ADR-3539: Count non-Actions app check runs outside the aggregator run window](3539-aggregator-app-checks-outside-run-window.md).
- [CI overview: Required-checks aggregator](../development/ci.md#required-checks-aggregator).
- `scripts/ci/tests/test_aggregator_edited_window.py`: edited-event window, boundary, draft-era exclusion, multiple `ready_for_review` transitions, and fail-closed timeline fixtures.
- Source: decision `vmafx-3459-aggregator-edited-design` (2026-11-15, aggregator edited-event window, paraphrased): add `edited` to `required-aggregator.yml` pull-request triggers and anchor its GitHub Actions check-run freshness window at the latest `ready_for_review` timeline event, or `pull_request.created_at` when never a draft, keeping the 2-minute margin.
