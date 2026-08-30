<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1123: Raise the Required-Checks-Aggregator deadline to 240 minutes and batch Docker digest updates

- **Status**: Accepted
- **Date**: 2026-08-30
- **Deciders**: Lusoris
- **Tags**: `ci`, `renovate`, `dependencies`, `gates`, `fork-local`

## Context

Between 2026-06-29 and 2026-08-30 no PR merged on this repository. The cause was
not a broken build: `origin/master` was green on all 26 required checks the whole
time. The cause was a self-reinforcing starvation loop in the merge gate.

`Required Checks Aggregator` (`.github/workflows/required-aggregator.yml`) is a
required status check. It polls the checks API and waits for every sibling
workflow on the same SHA to reach a terminal state. A sibling still `queued` or
`in_progress` when the poll deadline expires is treated as a **failure**.

The deadline was 90 minutes. That figure was itself a remediation: the header
comment records that a 30-minute deadline "failed every PR with `queued` for two
hours straight" on 2026-05-09, when roughly 80 CI jobs were queued across about
40 in-flight PRs.

The identical condition recurred at 90 minutes. A two-month Renovate backlog
opened ~44 concurrent dependency PRs. Each one fans out the full CI matrix, so
the queue depth again exceeded the deadline, and every aggregator on the wave
expired with **no real check failure**. Verified on three of them — for example
job `89636653774` ran 03:24:39 → 04:54:53Z (90 m 14 s) and expired on
`Build — Ubuntu gcc (CPU) + DNN: in_progress; CodeQL (Actions): queued;
ShellCheck + shfmt: queued`.

Because the aggregator is required, a timeout blocks the merge. Blocked merges
keep the PR open. Open PRs keep re-running CI on every master push. That keeps
the queue deep, which causes the next timeout. The loop does not drain on its
own, and six of the stuck PRs were `[SECURITY]` updates — including the
repository's only CRITICAL advisory (CVSS 9.1, `getkin/kin-openapi`), left
unmerged for two months.

A second, independent contributor to queue depth: Docker base-image digest
refreshes were the only dependency class with **no** grouping rule in
`renovate.json`. Every image opened its own PR and its own full matrix run —
debian, fedora, ubuntu, golang, archlinux, python, both distroless variants,
`docker/dockerfile`, `otel-collector` and `nvidia/cuda`. That is roughly a
quarter of the wave, for changes that are individually a one-line digest edit.

## Decision

Two coordinated changes:

1. **Raise the aggregator poll deadline from 90 to 240 minutes**, and lift the
   job's `timeout-minutes` from 100 to 250 so the polling job itself can still
   finish and report. The aggregator only polls the checks API — it runs no
   build — so a longer ceiling costs a runner slot rather than compute. The
   GitHub hosted-runner hard cap is 360 minutes, leaving headroom.

2. **Group all Docker digest/pin refreshes into a single Renovate PR**
   (`groupName: "Docker digests"`, `matchDatasources: ["docker"]`,
   `matchUpdateTypes: ["digest", "pin"]`, `automerge: true`). This mirrors the
   batching already in place for GitHub Actions, Go, Cargo and Python-patch
   updates, and removes roughly a quarter of the PR fan-out at the source.

These attack the two halves of the loop: (1) makes the gate survive a deep
queue, (2) makes the queue shallower.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Raise the deadline only | One-line change; directly fixes the observed timeouts | Leaves the fan-out that creates the depth; the next Renovate wave re-creates the queue and eventually outgrows 240 min too | Insufficient alone — treats the symptom, not the load |
| Group Docker digests only | Cuts the queue at source; no gate semantics touched | Does not help the wave already open, and a large enough batch of *any* dependency class can still exceed 90 min | Insufficient alone — does not unblock the current backlog |
| Make the aggregator non-required | Instantly unblocks every PR | Removes the single gate that guarantees required checks actually ran; a genuinely broken PR would merge | Rejected — trades a throughput problem for a correctness hole |
| Treat `queued`/`in_progress` at deadline as *pass* | Keeps the deadline short | Same correctness hole as above, but silent: the gate would report success having verified nothing | Rejected outright |
| `automerge` everything and let the queue drain unattended | No human in the loop | Unreviewed major version bumps reach `master`; the aggregator is still the thing that has to pass first, so it does not address the loop | Rejected — orthogonal to the actual failure |

## Consequences

**Positive.** Dependency PRs merge again, including the security backlog. The
gate now tolerates a queue depth roughly 2.7× what killed it. Docker digest
churn collapses from ~11 PRs per wave to 1.

**Negative.** A genuinely stuck sibling workflow now takes up to 4 hours to be
reported instead of 90 minutes, so a real hang is slower to surface. One runner
slot per in-flight PR is held for longer. A grouped Docker PR is
all-or-nothing: one bad image digest blocks the other ten in that batch, and the
fix is to let Renovate split the group or to pin the offending image.

**Neutral / follow-ups.** The deadline remains a wall-clock heuristic. The
structural fix is to stop polling — have each required workflow report into a
single aggregating check via `workflow_run`, so the gate is event-driven and has
no deadline at all. That is a larger redesign and is deliberately out of scope
here. If a future wave exceeds 240 minutes, prefer reducing fan-out (more
grouping) over raising the ceiling again.

## References

- `req` — user direction 2026-08-30: merge the outstanding version-bump PRs, and
  "there will be a ton of version bump PRs soon". This ADR is the change that
  makes that sustainable rather than a recurring manual sweep.
- `.github/workflows/required-aggregator.yml` — the deadline and its 2026-05-09
  precedent comment.
- [ADR-0037](0037-master-branch-protection.md) — master branch protection and the
  required-check set the aggregator gates.
- [ADR-0812](0812-renovate-go-rust-scheduling.md) — existing Renovate scheduling
  and grouping policy this extends.
- Evidence: aggregator job `89636653774` (90 m 14 s, expired with siblings
  `queued`/`in_progress`, no failing check) on PR #1089.
