<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3314: Pull-request workflows stay inside the Scorecard check-suite window

- **Status**: Accepted
- **Date**: 2026-10-14
- **Deciders**: Lusoris (operator decision vmafx-red-main-20261013-scorecard-plan)
- **Tags**: ci, scorecard, security, github-actions

## Context

The required `Scorecard PR Gate` (`scripts/ci/scorecard_gate.py`) fails below
an aggregate of 8.5. On 2026-10-13 master scored 8.404762 (882.5 of 1050
weighted points), and the SAST check stood at 7 of 10 although CodeQL runs on
every pull request.

OpenSSF Scorecard v5.6.0 decides whether a merged pull request was scanned by
reading the check suites of its head commit with the GraphQL field
`checkSuites(first: 30)`. Every workflow a `pull_request` event starts is one
check suite, and the GitHub Apps that report on a pull request add their own.
Master had 31 workflows with a `pull_request` trigger, so the CodeQL suite of
many pull requests sat past the thirtieth entry and Scorecard counted the
commit as unscanned.

Several of those workflows exist only to wait for a label (`ci: full`,
`autorelease: cut`, `run-observability-smoke`, `run-e2e-k8s`) and start no job
on most pull requests, yet each still creates a check suite. Three others
(`doxygen-public-api.yml`, `scorecard-policy.yml`, `upstream-consumers.yml`)
are single-lane workflows whose jobs fit an existing workflow.

## Decision

- `doxygen-public-api.yml` becomes a job of `docs.yml`,
  `scorecard-policy.yml` a job of `security-scans.yml`, and
  `upstream-consumers.yml` the `consumers` job of `ffmpeg-integration.yml`.
  Each moved job keeps its check name, so the required contexts and the
  aggregator list are unchanged.
- `ci-escalate.yml`, `observability-compose.yml` and `e2e-k8s.yml` lose their
  `pull_request` trigger and gain `workflow_call`. One new workflow,
  `pr-label-lanes.yml`, is the only workflow that listens for `labeled`; its
  three jobs call them under the same label, draft and same-repository
  conditions the callees used. The callees keep their schedule and
  `workflow_dispatch` triggers.
- `scripts/ci/pr_workflow_budget.py` counts the workflows whose `on:` names
  `pull_request` and fails above `BUDGET = 27`, which leaves three suites of
  the window to CodeQL and the GitHub Apps. It runs as a pre-commit hook and
  in `make lint-actions`; its tests plant one workflow over the budget and an
  unreadable workflow.

The count falls from 31 to 26. SAST is expected to move from 7 to 8 once the
merged pull requests Scorecard samples carry the new layout, about two to
three days after this lands; the aggregate then passes 8.5.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Do nothing and wait for an upstream Scorecard fix | No CI change | The gate stays red for an unknown time; every new workflow makes it worse | The window is a measured defect in the scanner, and the fix date is not ours |
| Lower the gate threshold or reweight SAST | One-line change | Hides the score drop; the operator ruled the threshold and gate math stay unchanged | Ruled out by the order |
| Fold only the three single-lane workflows | Smaller diff | Leaves 28 workflows, one above the budget, with no room for a new lane | Too close to the window |
| Fold every workflow into a few large ones | Many free suites | Loses per-workflow concurrency, permissions and readable check names; larger blast radius | Disproportionate to the gap |
| Budget of 30 | Matches the window exactly | The app suites count too, so CodeQL can still fall out | Gives no margin |

## Consequences

- **Positive**: CodeQL stays inside the window; a new pull-request workflow
  that would push it out fails locally before review.
- **Negative**: a new label-gated lane needs a `workflow_call` callee and a job
  in `pr-label-lanes.yml` instead of its own trigger.
- **Neutral / follow-ups**: the scanner defect itself is reported upstream
  through the fleet's post review; when Scorecard reads every suite, the
  budget can be revisited by a superseding ADR.

## References

- See [ADR-0535](0535-adr-atomic-allocator.md) for the original allocator design.
- See [ADR-0628](0628-adr-allocator-remote-aware.md) for the remote-aware extension.
- [ADR-2169](2169-ci-fewer-runs.md) for the label-gated lanes and the fewer-runs policy.
- Source: operator decision `vmafx-red-main-20261013-scorecard-plan` (paraphrased): fold the label-gated workflows into one pull-request workflow through `workflow_call` and hold the count with a budget gate that has a planted negative.
