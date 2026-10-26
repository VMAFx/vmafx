<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-3376: The PR type labeller runs on `pull_request`

- **Status**: Accepted
- **Date**: 2026-10-26
- **Deciders**: Lusoris
- **Tags**: ci, security, release

## Context

[`pr-type-label.yml`](../../.github/workflows/pr-type-label.yml) derives a
`type:*` label from a pull request's Conventional-Commit prefix
([ADR-1233](1233-github-native-release-notes.md)). It ran on
`pull_request_target` so that it could label pull requests from forks.
GitHub's default Actions event policy blocks `pull_request_target` on public
repositories unless an event policy allows it; the
[Actions policies documentation](https://docs.github.com/en/actions/concepts/about-actions-policies)
names 2026-12-02 as the enforcement date. The workflow is the only one in this
repository with that trigger. Fork pull requests are rare here (one of the
last 300), and ADR-1233 already treats a missing label as cosmetic: the pull
request lands under "Other changes" in the release body.

## Decision

We will run the workflow on `pull_request` with the same activity types. A
pull request from the repository itself keeps its label. A fork pull request
gets a read-only token under `pull_request`, so the job leaves a notice and
exits 0; a maintainer adds the label by hand. The workflow still checks out no
code and runs one labels API call.

## Alternatives considered

| Option | Pros | Cons | Verdict |
| --- | --- | --- | --- |
| `pull_request`, skip forks with a notice | No repository setting; no privileged trigger left | Fork pull requests need a manual label | Chosen |
| Keep `pull_request_target` and allow it through a repository event policy | Fork pull requests stay labelled | Needs a repository Actions setting kept outside the tree; keeps a privileged trigger | Rejected |
| `pull_request` plus a `workflow_run` labeller | Fork pull requests labelled without the blocked trigger | Second workflow and an artifact carrying the pull request number (`workflow_run.pull_requests` is empty for forks); more surface for a cosmetic label | Rejected |

## Consequences

- **Positive**: the repository holds no `pull_request_target` trigger, so the
  2026-12-02 policy changes nothing here and no event policy is needed.
- **Negative**: a fork pull request is not labelled automatically. This
  supersedes the fork note of ADR-1233's consequences.
- **Neutral**: the workflow now counts towards the pull-request workflow budget
  of `scripts/ci/pr_workflow_budget.py` (27 of 27).

## References

- [ADR-1233](1233-github-native-release-notes.md): the labeller and its release-note purpose.
- [ADR-2169](2169-ci-fewer-runs.md): unchanged, the workflow still does not run on `synchronize`.
- Operator decision `vmafx-pr-type-label-prt`, option a (paraphrased: switch to
  `pull_request`, skip fork pull requests with a notice).
