## ARC runner selector refuses forks (2026-10-12)

`ci/arc-fork-guard`, [ADR-3286](adr/3286-arc-runners-fork-guard.md).

- `.github/workflows/tests-and-quality-gates.yml` (Sanitizers) and
  `.github/workflows/lint-and-format.yml` (Cppcheck): the `runs-on` selector
  carries `(github.event_name != 'pull_request' ||
  github.event.pull_request.head.repo.full_name == github.repository)` between
  the variable and `'arc-runners'`. An upstream sync never touches these
  fork-only jobs' selectors; a rebase that rewrites either line keeps the guard.
- `scripts/ci/test_self_hosted_runner_workflow_contract.py`:
  `ArcForkGuardContract` admits only that form; a job moved to the pool copies
  the whole expression.
