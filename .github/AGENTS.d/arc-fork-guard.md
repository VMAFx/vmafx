---
paths:
  - .github/workflows/tests-and-quality-gates.yml
  - .github/workflows/lint-and-format.yml
  - scripts/ci/test_self_hosted_runner_workflow_contract.py
invariant: Every runs-on naming arc-runners carries the head-repo guard before the label; fork PRs stay hosted.
---
# ARC pool fork guard

[ADR-3286](../../docs/adr/3286-arc-runners-fork-guard.md), amends ADR-0359.

## Rule

- Admitted selector, whole, in one line (fallback `ubuntu-26.04` or
  `ubuntu-26.04-arm`); shown wrapped:

  ```text
  ${{ vars.ARC_RUNNERS_ENABLED == 'true'
      && (github.event_name != 'pull_request'
          || github.event.pull_request.head.repo.full_name == github.repository)
      && 'arc-runners' || 'ubuntu-26.04' }}
  ```

- Jobs today: Sanitizers, `Cppcheck (Whole Project)`. New pool job copies
  expression, adds row to `ARC_JOBS`.
- No job-level `if:` skip instead: both jobs required; fork PR needs them.

## Check

`ArcForkGuardContract` in
`scripts/ci/test_self_hosted_runner_workflow_contract.py` (`Tooling Tests`):
allow-list regex over every workflow line naming `arc-runners` or
`ARC_RUNNERS_ENABLED`; comment lines skipped. Planted refusals: unguarded,
guard joined with `||`, guard after label, constant compare, bare label.
