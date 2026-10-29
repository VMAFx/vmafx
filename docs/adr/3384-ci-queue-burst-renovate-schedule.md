# ADR-3384: Renovate updates inside its schedule; pull-request runs supersede

- **Status**: Accepted
- **Date**: 2026-10-28
- **Deciders**: Lusoris
- **Tags**: ci, renovate, governance

## Context

After the host reboot on 2026-10-28 one master push made Renovate rebase six
open dependency pull requests at once. Each head starts about 30 workflows, so
the account's queue held 268 queued runs at 18:28, and eleven green feature
pull requests waited behind them for hours. `renovate.json` already limits new
pull requests to the Monday schedule, but Renovate's `updateNotScheduled`
defaults to `true`: existing branches are rebased at any time, outside the
schedule. Three pull-request workflows (`docs.yml`, `pr-label-lanes.yml`,
`reuse.yml`) also had no workflow-level concurrency group, so an older push's
runs kept their runner slots after a newer push to the same pull request.

## Decision

`renovate.json` sets `updateNotScheduled: false` and lowers
`prConcurrentLimit` to 2 and `branchConcurrentLimit` to 3, so Renovate rebases
its branches only inside its schedule and at most two or three heads run their
suite at once. `reuse.yml` gets the ADR-1673 concurrency block. The concurrency
contract test (`scripts/ci/tests/test_master_concurrency_contract.py`) now also
refuses a workflow with a pull-request trigger that has no workflow-level group
which a newer push of the same pull request cancels. The two workflows that
cannot carry one are listed with a reason: `docs.yml` keeps per-job groups
(ADR-1294, ADR-1297; the Pages deploy must never be cancelled), and
`pr-label-lanes.yml` calls workflows that keep their own groups (one group at
both levels would deadlock).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Schedule plus `updateNotScheduled: false`, lower limits (chosen) | One setting removes the burst; security updates still come through the vulnerability alerts path | A conflicted Renovate branch waits for the next Monday window | Fits the existing weekly schedule |
| Group all Renovate updates into one pull request | One head per wave | One failing update blocks every other one; the repository's rules want one update per pull request for review | Larger blast radius |
| Path filters so dependency heads skip the heavy suites | Fewer runs per head | Lock and toolchain bumps are exactly the changes the heavy suites must see | Would hide real breakage |
| Do nothing; let the queue drain | No change | The next reboot or mass rebase repeats the outage | The cost recurs |

## Consequences

- **Positive**: a master push no longer starts a rebase wave of every open
  Renovate branch; superseded pull-request runs free their runners.
- **Negative**: a Renovate branch that conflicts on Tuesday stays conflicted
  until the next Monday window, unless a maintainer ticks its rebase box on the
  dependency dashboard (which Renovate honours outside the schedule).
- **Neutral / follow-ups**: a new pull-request workflow must carry a
  superseding group or a listed reason; the contract test says which.

## References

- Controller order `vmafx-ci-queue-burst-20261028` (paraphrased): cancel stale
  runs, audit concurrency and path filters, schedule the Renovate branches so
  one push wave is not six times thirty workflows.
- Renovate configuration options, `updateNotScheduled`.
- [ADR-1673](1673-master-runs-not-cancelled-by-concurrency.md),
  [ADR-2169](2169-ci-fewer-runs.md).
