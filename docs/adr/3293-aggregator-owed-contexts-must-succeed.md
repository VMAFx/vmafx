<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3293: Every owed required context must succeed; absent, skipped and neutral fail

- **Status**: Accepted
- **Date**: 2026-10-12
- **Deciders**: lusoris
- **Tags**: ci, governance

## Context

The `CI` workflow (`.github/workflows/required-aggregator.yml`) is the one
status check branch protection enforces. It reads every check run on the head
commit and decides whether the required names passed.

[ADR-0313](0313-ci-required-checks-aggregator.md) made "never reported" an
accepted outcome for every required name. At the time the build and test
workflows had trigger-level `paths-ignore` filters
([ADR-0341](0341-ci-paths-ignore-doc-only-prs.md)), so on a documentation-only
pull request their jobs never started, and the aggregator had to let them be
absent. `skipped` and `neutral` were accepted for the same reason.
[ADR-1151](1151-vmafx-first-release-1-0-0.md) later named three contexts that
had to report on a release branch, because there a missing build would have
let a release through.

The reason for the rule is gone. Since [ADR-1140](1140-ci-impact-planner.md)
every required workflow starts on every pull request and decides inside the
job, through the impact planner, whether it has work; a leg the diff does not
touch reports success. [ADR-2169](2169-ci-fewer-runs.md) added the CI tier,
which decides per event which required names are owed at all. What remained of
absent-means-pass was a hole: a required job that silently failed to start
(a workflow file error, a renamed job, a `needs` that skipped it, a runner
that never picked it up) passed the gate as if it had succeeded. The
controller's audit of 2026-10-12 listed this as finding A2-01.

## Decision

Every required name the aggregator lists must report `success` on the head
commit. A name that is absent, `skipped`, `neutral`, `cancelled` or failed
blocks the merge, unless one of exactly three exemptions applies. Each
exemption is decided in the aggregator's own run and covers only absent or
skipped; a context that ran and failed always blocks.

1. **Not owed by the tier** ([ADR-2169](2169-ci-fewer-runs.md)): the CI tier of
   this event does not owe the name. A name that ran anyway must still pass.
2. **Pull-request-only on another event**: the names in `pullRequestOnly`
   exist only on a pull request (workflows with only a `pull_request`
   trigger, `Release Dry Run`, `Dependency Review`, and the code-scanning
   results that land as alerts on a push). On a push or a dispatch each may be
   absent or skipped and is named in the log. On a pull request each is owed.
3. **Hardware lane switched off** ([ADR-1177](1177-sycl-arc-self-hosted-runner.md),
   [ADR-1319](1319-fail-closed-self-hosted-gpu-admission.md)): `Coverage GPU` and
   `SYCL Parity (Arc A380)` may be absent or skipped while their repository
   switch is off. With the switch on they must succeed.

`strictMustReport`, the Scorecard gate of the event and the tier-decision
check keep their existing handling, which was already success-only.
ADR-1151's `releaseMustReport` list is removed: it is a special case of the
general rule now.

This decision supersedes in part ADR-0313 (absent-means-pass), ADR-0341 (the
trigger-level `paths-ignore` it describes no longer exists, and the comments
that still cited it are removed) and ADR-1151 (the `releaseMustReport` list).

## Measured

- Required workflows with a trigger path filter on `pull_request`: none. The
  only `pull_request` workflow with a path filter is `upstream-consumers.yml`,
  which is not required.
- Master pushes of 2026-10-12: the names absent from the head commit were the
  `pullRequestOnly` names and no other required name. The exemption list is
  that set.
- `scripts/ci/tests/test_aggregator_absent_fails.py` runs the aggregator's
  embedded script through `scripts/ci/required_aggregator_harness.py`. Against
  the aggregator before this change (`bfd2074e9`,
  `AGGREGATOR_UNDER_TEST=<copy>`): 9 failed, 8 passed. Against this change,
  together with the existing aggregator tests
  (`test_ci_aggregator_tier.py`, `test_required_release_legs.py`,
  `test_go_workflow_contract.py`): 46 passed, 257 subtests passed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep absent-means-pass (ADR-0313) | No change | A required job that never starts passes the gate | The trigger filters it existed for are gone; the hole is all that remains |
| Fail absent, keep `skipped` / `neutral` accepted | Smaller change | A job skipped by a broken `needs` or `if` still passes; that is the same hole one step later | The planner already reports success for an untouched leg, so a skip is never the expected answer |
| Grow per-branch lists like `releaseMustReport` | Targeted | Every new case needs a new list; the default stays unsafe | Inverting the default removes the need for lists |
| **Success required, three named exemptions decided in the run** (chosen) | A missing or skipped job blocks; each exemption is visible in the log and tested | The exemption lists must be kept current when a workflow's triggers change | — |

## Consequences

### Positive

- A required job that fails to start, is renamed, or is skipped by a broken
  condition blocks the merge instead of passing silently.
- Each exemption is named in the aggregator's log for the run that used it.
- The planted test keeps the rule: it fails against the old aggregator.

### Negative

- A workflow that gains a path filter on `pull_request` or loses its push
  trigger turns its required names red until the filter is removed or the name
  joins `pullRequestOnly`. This is intended, but it is a new way to block a
  merge.

### Neutral / follow-ups

- Hardware lanes gated on undefined repository variables are audit finding
  A2-02 and get their own fix.

## References

- Controller order `fleet-green-while-broken-20261012`, row A2-01: the
  aggregator lets absent required contexts pass; make an owed context fail
  unless it reports success, and prove the fix with a planted failure kept as a
  test (paraphrased).
- [ADR-0313](0313-ci-required-checks-aggregator.md),
  [ADR-0341](0341-ci-paths-ignore-doc-only-prs.md),
  [ADR-1140](1140-ci-impact-planner.md),
  [ADR-1151](1151-vmafx-first-release-1-0-0.md),
  [ADR-1687](1687-required-release-dry-run-legs.md),
  [ADR-2169](2169-ci-fewer-runs.md).
