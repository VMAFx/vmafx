# CI Draft Auto-Merge Gate

## Question

Why did ready PRs merge while visible checks were not green, and what should
the CI gate change so the train cannot repeat that failure?

## Findings

The merge train PRs had completed skipped check runs from their draft state.
GitHub branch protection treats a skipped check as successful. Because the
single required context, `Required Checks Aggregator`, was itself skipped on
draft PRs, enabling auto-merge immediately after `gh pr ready` allowed GitHub
to merge before the new ready-for-review workflows registered.

The later ADR Number Collision Guard failures were a secondary symptom. The
collision job compared added ADR prefixes against live `origin/master`; after
the PR had already merged, live master contained the PR's own ADR, so the job
reported a false self-collision.

## Implementation Notes

- `required-aggregator.yml` now runs on draft PRs and fails explicitly instead
  of being skipped.
- The aggregator ignores stale sibling check runs created before the current
  workflow's registration window and selects the newest check run per name.
- The aggregator waits through a short registration grace period before
  treating absent sibling checks as legitimate path-filter skips.
- The aggregator grants `actions: read` because it reads its own workflow-run
  creation timestamp as the stale-check cutoff anchor.
- `rule-enforcement.yml` ADR collision phase 1 now checks the event `BASE_SHA`
  tree rather than live `origin/master`.

## Validation

Local validation covers YAML syntax by parsing both changed workflows with
Python's YAML loader, plus the standard ADR/index checks. The live behavioral
gate requires the PR to be opened as draft, then marked ready, because GitHub's
branch-protection behavior is server-side.
