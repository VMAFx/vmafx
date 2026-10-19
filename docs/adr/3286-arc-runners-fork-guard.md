<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3286: Pull requests from forks never reach the ARC runner pool

- **Status**: Accepted
- **Date**: 2026-10-12
- **Deciders**: lusoris
- **Tags**: ci, security, infra

## Context

[ADR-0359](0359-arc-runners-pilot.md) routes two jobs, Sanitizers in
`tests-and-quality-gates.yml` and `Cppcheck (Whole Project)` in
`lint-and-format.yml`, to the self-hosted `arc-runners` scale set when the
repository variable `ARC_RUNNERS_ENABLED` is `true`. The selector looked at the
variable alone, so with the variable set a pull request from a fork would run
its own code on a pod in the maintainer's cluster. The variable is unset today
and nothing has run there; it must not be set before the selector refuses
forks.

The repository already keeps forks off self-hosted hardware elsewhere: the SYCL
runner probe in `sycl-parity.yml` runs only for a push, a dispatch or a pull
request whose head repository is this repository
([ADR-0331](0331-skip-ci-on-draft-prs.md)). Approving a fork's workflow runs
does not change that: an approval lets the fork's code run, it does not choose
where.

## Decision

Both `runs-on` selectors add the head-repository condition between the switch
and the label:

```yaml
runs-on: ${{ vars.ARC_RUNNERS_ENABLED == 'true' && (github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository) && 'arc-runners' || 'ubuntu-26.04' }}
```

A push, a dispatch, a schedule and a pull request from a branch of this
repository go to the pool when the variable is `true`; a pull request from a
fork always runs on the hosted fallback. `ArcForkGuardContract` in
`scripts/ci/test_self_hosted_runner_workflow_contract.py` (required check
`Tooling Tests`) reads every workflow and admits a line that names
`arc-runners` or `ARC_RUNNERS_ENABLED` only in exactly this form, with a hosted
`ubuntu-26.04` or `ubuntu-26.04-arm` fallback. It is an allow-list: a selector
written another way is refused, not parsed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Guard in `runs-on` (chosen) | The job still runs for a fork, on a hosted runner; one expression per job; the check reads one line | The expression is long | — |
| Job-level `if:` that skips the job for forks | Shortest expression | Sanitizers and Cppcheck are required checks; a skipped job would leave a fork's pull request without them | A fork's pull request needs the same checks |
| Rely on the approval of fork workflow runs | No workflow change | Approval decides whether the code runs, not where; one approval would put a fork's code on the cluster | Does not keep forks off the pool |
| Restrict the scale set to this repository's branches in ARC | Enforced outside the workflow | ARC selects by label, not by the event's head repository; the cluster cannot see where a pull request came from | Not expressible there |
| A deny-list check for unguarded selectors | Shorter check | Misses forms nobody listed (the guard after the label, joined with `\|\|`) | The allow-list refuses every other form |

## Consequences

- **Positive**: `ARC_RUNNERS_ENABLED` can be set without exposing the cluster to
  a fork's code. A job added to the pool later has to carry the guard, or
  `Tooling Tests` fails.
- **Negative**: the selector is longer; a third job copies it whole.
- **Neutral / follow-ups**: fork pull requests keep the hosted runners' queue
  time when the pool is on. [CI runner pools](../development/ci-runners.md)
  shows the expression.

## References

- Source: operator order of 2026-10-12, ledger id `vmafx-arc-fork-guard`
  (paraphrased): add the head-repository guard to the Sanitizers and Cppcheck
  jobs in one small pull request, with a test that has a planted negative case,
  before `ARC_RUNNERS_ENABLED` is set.
- [ADR-0359](0359-arc-runners-pilot.md) (the pilot),
  [ADR-0331](0331-skip-ci-on-draft-prs.md) (the existing fork guard of the SYCL
  runner probe), [ADR-3251](3251-hosted-runner-label-explicit.md) (hosted
  fallback labels).
