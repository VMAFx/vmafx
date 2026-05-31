<!-- markdownlint-disable MD060 -->
# ADR-0679: CI Draft Auto-Merge Gate

- **Status**: Accepted
- **Date**: 2026-05-22
- **Deciders**: Lusoris, Codex
- **Tags**: ci, github-actions, merge-train, adr, fork-local

## Context

Draft PRs intentionally skip expensive CI jobs so the merge train can keep a
large stack of work without saturating GitHub runners. The fork relies on
`Required Checks Aggregator` as the single required branch-protection status,
with sibling workflows allowed to be `success`, `skipped`, or `neutral` for
path-filtered cases.

The AI manifest train exposed a race. A draft PR had completed skipped check
runs on the same head SHA. When the PR was marked ready and auto-merge was
enabled immediately, GitHub could consider the skipped draft-era required
context sufficient before the fresh ready-for-review workflow run had
registered. The PR then merged while real CI was still queued or in progress.
The ADR collision guard also compared against live `origin/master`; after a
premature merge, that job could fetch a master that already contained the PR's
own ADR and report a false self-collision.

## Decision

Make `Required Checks Aggregator` run on draft PRs and fail explicitly with a
clear message. Do not use a job-level draft skip on the one required context.
When the PR is later marked ready, the same workflow reruns and can replace
the draft failure with the real aggregate result.

In the aggregator script, ignore stale sibling check runs created before the
current workflow run's registration window and select the newest run per check
name. This prevents draft-era skipped jobs on the same commit from masking
new ready-for-review queued, in-progress, or failed checks. Keep a short
registration grace period before treating missing checks as path-filter skips.
Grant the workflow `actions: read` so it can read its own run creation time for
that stale-check cutoff.

In the ADR collision guard, compare phase 1 against the event `BASE_SHA`
instead of live `origin/master`. The branch base is the correct pre-merge
collision target and avoids post-merge self-collision noise.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fail the required aggregator on drafts and ignore stale draft-era check runs | Blocks auto-merge until ready CI genuinely runs; preserves draft runner savings for expensive jobs | Draft PRs show one red required check by design | Chosen: it fixes the unsafe branch-protection signal directly |
| Keep the workflow unchanged and only wait manually before enabling auto-merge | No code change | Human discipline can drift and does not protect future agents or UI auto-merge | Rejected: the required check itself must be unambiguous |
| Remove `skipped` from accepted sibling conclusions | Stronger gate | Breaks legitimate path-filter/doc-only semantics from ADR-0313 | Rejected: this would revive the doc/Python-only deadlock |
| Compare ADR collisions against live `origin/master` | Catches concurrent merges at job runtime | Produces false self-collisions after fast merges | Rejected: the PR base SHA is the deterministic comparison point |

## Consequences

- **Positive**: Draft PRs can no longer satisfy the single required status via
  skipped check runs, and auto-merge waits for a real ready-for-review
  aggregate result.
- **Positive**: ADR collision failures no longer turn red after a PR has
  already merged because the job fetched a moved master branch.
- **Negative**: Draft PRs will display a deliberate failed aggregator check
  until they are marked ready.
- **Neutral / follow-ups**: Do not enable auto-merge on train PRs until the
  fixed aggregator is present on `master`; keep draft descendants draft and
  promote one PR at a time.

## References

- [ADR-0313](0313-ci-required-checks-aggregator.md) — required checks
  aggregator.
- [ADR-0331](0331-skip-ci-on-draft-prs.md) — draft PR CI skip policy.
- [ADR-0386](0386-adr-numbering-collision-prevention.md) — ADR number
  collision guard.
- Source: req — "why are they all merged while not green"
