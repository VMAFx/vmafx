<!-- markdownlint-disable MD060 -->
# ADR-0331: Skip CI on draft pull requests

- **Status**: Accepted
- **Date**: 2026-05-08
- **Deciders**: lusoris
- **Tags**: ci, build, fork-local

## Context

The fork commonly has 10+ draft pull requests in flight at any one
time, each rebased onto `master` repeatedly during review and bundling.
The full CI matrix triggered on every push (~19 required checks plus
several advisory lanes) currently fires on those draft pushes too,
which doubles the CI bill against work the author still considers
in-progress. The maintainer flagged the cost directly and asked for a
single fix that stops the bleed across every workflow.

## Decision

We will gate every fork workflow that triggers on `pull_request` so it
no longer runs while the PR is in `draft` state. Concretely each
affected `pull_request` block lists
`types: [opened, synchronize, reopened, ready_for_review]` and every
top-level job carries
`if: github.event_name != 'pull_request' || github.event.pull_request.draft == false`.
The `ready_for_review` event is the trigger that re-fires CI when an
author marks a draft as ready, replacing the lost `synchronize`
events. The second clause keeps `push:` triggers (where there is no
PR object) intact.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Per-job draft gate (chosen) | Single uniform pattern across 8 workflows; safe fallback for `push:` events; honours GitHub's `ready_for_review` semantics so promotion fires CI exactly once. | Adds two lines per job; aggregator skip leaves draft PRs in "missing required check" state, but drafts are unmergeable by design so the gate is moot. | — |
| Concurrency-only (cancel old runs but still fire new ones) | Already in place for most workflows. | Does not stop the run, only deduplicates within a ref. Each draft push still consumes one full matrix. | Rejected — does not address the cost driver. |
| No gate, rely on rebase-before-merge | Zero workflow changes. | The user explicitly asked for a CI-skip fix; ignoring the request and waiting for rebase doubles spend on every iteration. | Rejected per direct user request. |

## Consequences

- **Positive**: Draft PRs no longer consume CI minutes for the
  `pull_request` event. Promotion to ready-for-review fires the full
  matrix once via `ready_for_review`; subsequent `synchronize` events
  on the now-ready PR fire CI as before. `push:` events on `master`
  are unaffected.
- **Negative**: A draft PR shows "Required check missing" against
  branch protection, but GitHub already blocks merging a draft PR by
  definition, so there is no merge-path regression. Developers cannot
  preview their CI status while the PR is still draft — they must
  promote to ready (or open a non-draft PR) to see green checks.
- **Neutral / follow-ups**: `docs/development/ci.md` documents the
  gate so a future contributor opening a draft PR understands why no
  checks fire. ADR-0313's `Required Checks Aggregator` already lists
  `ready_for_review` in its `types`; this ADR codifies the same
  pattern across the rest of the workflow set.

## References

- Source: `req` — direct user direction (paraphrased): "stop GitHub
  Actions CI from running on draft pull requests across all 8
  workflows; ~14 drafts are in flight and each one rebases before
  merge, doubling the CI bill."
- [ADR-0313](0313-ci-required-checks-aggregator.md) — the aggregator
  workflow already adopted the `ready_for_review` event; this ADR
  generalises the pattern.
- [ADR-0317](0317-ci-doc-only-pr-flake-fix.md) — companion CI cost
  fix (path filters for Docker / FFmpeg integration); same theme,
  different mechanism.
