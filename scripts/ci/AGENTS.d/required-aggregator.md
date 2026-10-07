---
paths:
  - scripts/ci/check-aggregator-names.sh
  - scripts/ci/tests/test-check-aggregator-names.sh
  - scripts/ci/required_aggregator_harness.py
  - scripts/ci/test_go_workflow_contract.py
  - scripts/ci/tests/test_required_release_legs.py
invariant: `required` list = `# required-aggregator` markers; one reporter per required name; contract suites share one harness.
---
<!-- markdownlint-disable MD013 MD060 -->
# Required aggregator: check names and the shared harness

## check-aggregator-names.sh invariants

- Gates 1:1 parity between required status checks declared in
  `.github/workflows/required-aggregator.yml` (`const required = [...]`) and
  `# required-aggregator` markers on `name:` fields across workflow files.
- Enforced locally via `make lint-sh` and pre-commit hook `check-aggregator-names`.
- Display names must stay concise ($\le 30$ chars) per `docs/development/ci-job-names.md`.

## Workflow coupling

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `check-aggregator-names.sh` | `check-aggregator-names` pre-commit hook; `rule-enforcement.yml` — `Required check names have one reporter each (ADR-1259)` (gate + `tests/test-check-aggregator-names.sh`) | Two invariants: aggregator's `required` list equals `# required-aggregator`-marked names; each required name is reported by exactly one job (aggregator keeps only newest run per name, so shared name lets one job mask other's failure — `Windows MSVC+CUDA`, fixed 2026-09-19). `job_names()` skips everything under `steps:` key and workflow's top-level `name:`; change that parser and fixture test together. new lane must not reuse required name. |

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `required_aggregator_harness.py` | Shared by `test_go_workflow_contract.py` and `test_sycl_tidy_workflow_contract.py` | Owns one Node.js driver for executing embedded aggregator against synthetic check results. Keep both contract suites on this harness so polling-time simulation and result decoding cannot drift. |
| `test_go_workflow_contract.py` | `rule-enforcement.yml` — `Verify Go required-check contract` | Uses shared aggregator harness for Go pass/fail outcomes; guards ready-event coverage and step-level `go_checks` routing; pins early, non-mutating `go fix -diff ./...` gate and matching Make targets. Keep it before authoring exemptions (ADRs 1238 and 1338). |

## Release legs (ADR-1687)

- `Tester Image`, `Windows Tester Zip` and `Release Dry Run` sit in `required`,
  `strictMustReport` and `delayedStrictDependencies` together; each gate needs
  last pull-request job of its chain (`build`, `verify`, dry-run plan plus
  its three groups). `Release Dry Run` is also in `pullRequestOnly`, which drops
  it from non-pull-request run because `release-dry-run.yml` has no push trigger.
  A dependency name aggregator waits on must stay unique across workflows:
  that is why two tester `validate` jobs are not called `Validate source`.
  `tests/test_required_release_legs.py` plants each removal and executes
  aggregator for pull request and push.
- `required_aggregator_harness.py` strips whole-line `//` comments from
  `required` block before it reads names, as `check-aggregator-names.sh`
  does. Without that, apostrophe in comment silently drops every later name
  from synthetic check list, and test of such name passes or fails for
  wrong reason. `event` argument selects `pull_request` or `push`.
