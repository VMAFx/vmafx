<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3367: Workflow job time limits cover the sum of their step limits

- **Status**: Accepted
- **Date**: 2026-10-22
- **Deciders**: Lusoris (operator decision vmafx-ci-apt-budget)
- **Tags**: ci, github-actions, timeouts

## Context

PR #2927 raised the `Install build deps` step limit of `MCP Smoke` and `Tiny AI`
in `.github/workflows/tests-and-quality-gates.yml` from 8 to 20 minutes, so a
slow package mirror no longer cancels a healthy run in that step. The job
limits stayed at 35 (`MCP Smoke`) and 45 (`Tiny AI`) minutes. On 2026-10-22 the
apt step of `MCP Smoke` took 19 minutes 59 seconds, inside its limit, and the
job reached its own 35-minute limit in the libvmaf build (job 119986586497):
the step limit was raised, but the job could not hold it.

Measured over the successful jobs of the last 60 `tests-and-quality-gates.yml`
runs (2026-10-22):

| Job | Successful jobs | Longest total | Longest apt step | Longest of the other steps |
| --- | --- | --- | --- | --- |
| `MCP Smoke` | 44 | 24.1 min | 5.0 min | 22.1 min |
| `Tiny AI` | 38 | 35.1 min | 14.9 min | 27.4 min |

Nothing in the tree compared a job's limit with the limits of its steps.

## Decision

- A job's `timeout-minutes` is at least the sum of its steps' `timeout-minutes`
  plus 1.25 times the longest measured healthy run of its other steps, the
  margin rule of the engine's job-timeout gate.
- `Tiny AI`: 20 + 1.25 x 27.4 = 54.3, limit 55 minutes. `MCP Smoke`:
  20 + 1.25 x 22.1 = 47.6, limit 55 minutes, rounded up to `Tiny AI`'s value.
  The comment above each limit names the measurement.
- `scripts/ci/check_job_timeout_budget.py` fails when any job under
  `.github/workflows/` has a `timeout-minutes` below the sum of its steps'
  limits. A job without a limit counts as GitHub's 360 minutes; a limit that is
  not a whole number (an expression) fails the run, since it cannot be checked.
  It runs in `make lint-actions` and as a pre-commit hook pair;
  `scripts/ci/tests/test_check_job_timeout_budget.py` plants a job below its
  step sum and checks that it is refused.

## Alternatives considered

| Option | Pros | Cons | Verdict |
| --- | --- | --- | --- |
| Raise the two job limits only | Smallest change | The next step-limit raise repeats the incident; nothing catches it | Rejected |
| Check the full rule (steps plus 1.25 x measured rest) | Holds the whole margin | Needs run history the check cannot read offline; a lint gate would call the API | Rejected; the measurement stays in the comment |
| Check the step sum only, size the margin by hand (chosen) | Offline, deterministic, catches the class of the incident | The margin for uncapped steps is the author's measurement, not checked | Chosen |
| Drop the apt step limit | No sum to hold | A stalled mirror would run to the job limit without a named step | Rejected |

## Consequences

- A step-limit raise that the job cannot hold fails locally and in the lint
  job, before a run is cancelled.
- The two jobs can run 20 minutes longer than before when a mirror is slow; a
  real hang still ends at the apt step's 20 minutes.
- Caching the apt archives (operator decision vmafx-ci-apt-budget, part b)
  is a separate change.

## References

- [ADR-3314](3314-pr-workflow-budget-scorecard-window.md) for the sibling workflow budget gate this check follows.
- Source: operator decision `vmafx-ci-apt-budget` (paraphrased): size each job limit as the apt step limit plus 1.25 times the measured longest healthy build, 55 minutes for `MCP Smoke` and `Tiny AI`, and add a check with a planted case that every job limit covers its step limits; caching apt archives follows as its own change.
- Source: operator decision `engine-job-timeout-gate-20261018` (paraphrased): the 1.25 times measured maximum margin for job limits.
