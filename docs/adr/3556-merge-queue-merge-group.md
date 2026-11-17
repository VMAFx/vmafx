<!-- markdownlint-disable MD013 MD025 MD060 -->
# ADR-3556: Trigger required checks and aggregator on merge-queue merge_group events

- **Status**: Accepted
- **Date**: 2026-11-16
- **Deciders**: Project maintainer
- **Tags**: ci, fork-local

## Context

Branch protection on `master` requires one status check, `Required Checks Aggregator` ([ADR-0313](0313-ci-required-checks-aggregator.md), [ADR-1297](1297-ci-gate-every-reporting-check.md)). When `master` merges pull requests through a GitHub merge queue, GitHub dispatches `merge_group` events (`types: [checks_requested]`) on synthetic queue refs (`refs/heads/gh-readonly-queue/master/...`), setting `GITHUB_SHA` to the merge group's head commit and `GITHUB_REF` to the queue ref. Workflows that trigger only on `pull_request` and `push` never start on a merge group, leaving `Required Checks Aggregator` and its owed check contexts absent so the merge queue stalls.

Two required contexts cannot run on a merge group. `Scorecard Master Gate` (`.github/workflows/scorecard.yml`) publishes OpenSSF Scorecard results (`publish_results: true`, allowed only on the default branch) and checks `git/ref/heads/master` (`scripts/ci/scorecard_gate.py master`). `Scorecard PR Gate` (`.github/workflows/security-scans.yml`) requires a pull-request event payload and already ran on the pull request before queueing. Meanwhile, steps and jobs that publish packages, push container images, deploy Pages, sign releases, or write to the repository must remain confined to their existing release, dispatch, or `master` push events and never execute on a speculative merge group.

## Decision

Merge pull requests into `master` through a GitHub merge queue (the `master` ruleset `merge_queue` rule is applied separately through the GitHub rulesets API with read-back after this change lands, so `merge_group` runs start only from then on), and add `merge_group` (`types: [checks_requested]`, `branches: [master]`) to `.github/workflows/required-aggregator.yml` and to every calling workflow that produces a required context owed on `merge_group` (`.github/workflows/praetor-api.yml`, which is byte-locked to the pinned Praetor asset, keeps its existing bare `merge_group:` trigger):

- Keep `merge_group` on the `full` CI tier (`scripts/ci/ci_tier.py`) and full impact plan (`scripts/ci/plan-ci-impact.py`), without adding `merge_group` routing to either script.
- In `.github/workflows/required-aggregator.yml`, define an event-scoped `mergeGroupExempt` list holding `'Scorecard Master Gate'` next to `pullRequestOnly`. On `context.eventName === 'merge_group'`, exempt `pullRequestOnly`, `'Scorecard PR Gate'` (already dropped off `pull_request`), and `mergeGroupExempt`, logging each exempt context via `core.info` (`Scorecard Master Gate` runs on the `master` push after the merge, and `Scorecard PR Gate` ran on the pull request). Keep `push` and `pull_request` (including `edited`, [ADR-3551](3551-aggregator-edited-window.md)) behaviour unchanged.
- Extend job and step guards on the path to owed `merge_group` contexts (`sanitizers.yml` `asan-ubsan`, `sycl-parity.yml` `runner-available`, `tests-and-quality-gates.yml` `gpu-full-runner-available`, `windows-tester-bundle.yml` `validate` ref and source resolution, and `lint-and-format.yml` diff-base steps reading `github.event.merge_group.base_sha`).
- Exclude `merge_group` alongside `pull_request` on the `docker-publish-tester.yml` `build-gpu` runner-cost guard so the three tester GPU image builds wait for the post-merge `master` push, and refuse any `pull_request` cost guard lacking a `merge_group` exclusion in `scripts/ci/tests/test_merge_group_workflow_contract.py`.
- Keep `release-dry-run.yml`, `rule-enforcement.yml`, `scorecard.yml`, and every publish, deploy, registry-push, release-signing, or repository-write step on their existing events only.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Option a: trigger `required-aggregator.yml` and feeding workflows on `merge_group` (`checks_requested`, `branches: [master]`), exempt `Scorecard Master Gate` via `mergeGroupExempt` on `merge_group`, and keep tier `full` and existing publish gates | Evaluates the combined merge group under the full CI tier; preserves single-context ruleset enforcement, `push` parity, and publish safety | Runs the full CI matrix on each merge group before `master` lands | Chosen; satisfies GitHub merge-queue requirements without weakening `master` verification or triggering publish jobs |
| Option b: route `merge_group` through the `light` tier or diff-routed `plan-ci-impact.py` | Shorter queue runtime when queued pull requests touch narrow paths | Skips full-tier cross-PR integration legs on the combined merge candidate | Rejected: `ci_tier.py` and `plan-ci-impact.py` fail closed to `full` off own pull requests so combined merge candidates run every owed check |
| Option c: trigger `scorecard.yml` on `merge_group` instead of exempting `Scorecard Master Gate` | Zero `merge_group`-specific exemptions in `required-aggregator.yml` | `ossf/scorecard-action` refuses `publish_results: true` outside the default branch, and `scorecard_gate.py master` inspects `refs/heads/master` rather than the queue ref | Rejected: incompatible with default-branch Scorecard publishing and ref lookup |
| Option d: require individual build and test contexts directly in the `master` ruleset | Avoids `merge_group` handling in `required-aggregator.yml` | Breaks [ADR-0313](0313-ci-required-checks-aggregator.md) single-context branch protection and hardware-lane operator switches | Rejected: violates ADR-0313 |

## Consequences

- **Positive**: the `master` ruleset can require `Required Checks Aggregator` inside a GitHub merge queue; every workflow producing an owed `merge_group` context starts on `checks_requested` for `master`, evaluates the combined queue head under tier `full`, rejects foreign `pull_request` check runs on the same SHA, and blocks queue merges when any owed context is absent or red. Publish, Pages, and release jobs remain confined to their existing triggers.
- **Negative**: merge-group runs execute the full CI tier across 21 feeding workflows before commits reach `master`, and adding a new required context outside `pullRequestOnly` requires `merge_group` on its calling workflow (enforced by `scripts/ci/tests/test_merge_group_workflow_contract.py`).
- **Neutral / follow-ups**: `Scorecard Master Gate` continues to run and enforce its gate on the `master` push immediately after the merge queue lands a group, and `Scorecard PR Gate` continues to run on every pull request before queueing. `scripts/ci/tests/test_aggregator_event_scope.py` covers `merge_group` pass, missing/failing context, Scorecard and `pullRequestOnly` boundary exemptions, and foreign `pull_request` run rejection; `scripts/ci/tests/test_merge_group_workflow_contract.py` enforces the feeding workflow trigger set and forbids `merge_group` on `release-dry-run.yml`, `scorecard.yml`, and `rule-enforcement.yml`.

## References

- [ADR-0313: Single required-checks aggregator](0313-ci-required-checks-aggregator.md).
- [ADR-1297: Gate every reporting check in Required Checks Aggregator](1297-ci-gate-every-reporting-check.md).
- [ADR-2169: Two-tier CI check contract](2169-ci-fewer-runs.md).
- [ADR-3293: Every owed required context must succeed](3293-aggregator-owed-contexts-must-succeed.md).
- [ADR-3551: Anchor edited pull-request aggregator runs at ready-for-review](3551-aggregator-edited-window.md).
- [CI overview: Merge queue](../development/ci.md#merge-queue).
- `scripts/ci/tests/test_aggregator_event_scope.py` and `scripts/ci/tests/test_merge_group_workflow_contract.py`.
- Source: maintainer request (paraphrased; vmafx-merge-throughput-queue-20261116, 2026-11-16, merge queue): add `merge_group` (`checks_requested` on `master`) to `required-aggregator.yml` and every workflow producing an owed `merge_group` context, exempt both Scorecard gates on `merge_group` while keeping tier `full` and existing `push` and `pull_request` behaviour, and keep publish steps on their prior events.
