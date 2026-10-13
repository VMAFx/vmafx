<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3294: Every CodeQL language is analysed on every event

- **Status**: Accepted
- **Date**: 2026-10-13
- **Deciders**: lusoris
- **Tags**: ci, security

## Context

`.github/workflows/security-scans.yml` runs three CodeQL jobs: C/C++
(`codeql-cpp`), Python (`codeql-python`) and GitHub Actions
(`codeql-actions`). Under [ADR-1140](1140-ci-impact-planner.md) the C/C++ and
Python jobs ran the impact planner and skipped `init` and `analyze` when the
diff did not touch the `c_core` or `python_lint` selector.
[ADR-1389](1389-codeql-actions-universal-pr-sast.md) had already made the
Actions job unconditional and kept the other two diff-aware.

GitHub's own `CodeQL` check compares the analyses uploaded for the head
commit with the categories the base branch has. When a category of the base
is missing from the head, it reports `neutral` with "N configuration not
found". Master carries all three categories from earlier runs, so every pull
request that skips one of the two diff-aware jobs gets a neutral `CodeQL`
check.

`CodeQL` is a required name in `pullRequestOnly` of the required-checks
aggregator. Since [ADR-3293](3293-aggregator-owed-contexts-must-succeed.md) an
owed name must report `success`, and `neutral` blocks. Every pull request that
does not touch both `core/` and Python code was therefore blocked, for example
pull requests #2760, #2761, #2765 and #2767.

## Decision

`codeql-cpp`, `codeql-python` and `codeql-actions` analyse their language on
every event the workflow runs for. Their `init` and `analyze` steps carry no
`if:`, and the two jobs no longer run the impact planner. The job-level
`needs: tier` / `if: needs.tier.outputs.light == 'true'`, which only leaves out
draft pull requests, stays.

The `python_lint` selector of `.github/ci-impact.json` had CodeQL Python as its
only consumer and is removed.

This decision supersedes [ADR-1140](1140-ci-impact-planner.md) in part (the
CodeQL legs) and extends [ADR-1389](1389-codeql-actions-universal-pr-sast.md)
to the C/C++ and Python legs. The aggregator rule of ADR-3293 is unchanged.

## Measured

- Pull request #2765, head `e8d7c7fc8df5`: check run 115248176264, `CodeQL`,
  conclusion `neutral`, "1 configuration not found"; the missing configuration
  is `security-scans.yml` category `/language:python`. The pull request touched
  only a test file under `core/test/`, a changelog fragment, a rebase note and
  `docs/state.md`.
- Duration on master pushes, from the `security-scans.yml` runs of 2026-10-12
  and 2026-10-13: CodeQL C/C++ took 17 to 26 minutes where it ran (runs
  38290002457, 38295067150, 38301871566, 38377429291), CodeQL Python 1 to 2
  minutes, CodeQL Actions about 1 minute. The jobs run in parallel with the
  rest of CI.
- `scripts/ci/test_security_workflow_contract.py` gains
  `test_codeql_analyses_every_language_on_every_event` and a planted-mutation
  test. Against the workflow before this change the first test fails with four
  problems each for `codeql-cpp` and `codeql-python`; on this change the file
  passes (6 passed, 3 subtests).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Accept `neutral` for `CodeQL` in the aggregator | No extra runner time | A real "configuration not found" (a broken or renamed analysis) passes again; it reopens the hole ADR-3293 closed | Loosens the check instead of fixing the cause |
| Upload an empty SARIF for a skipped language | Cheap | Reports a language as analysed when it was not; the alert state of the base is compared with nothing | A result that claims work which did not happen |
| **Analyse every language on every event** (chosen) | `CodeQL` reports a real result for every category; no special case anywhere | 17 to 26 minutes more parallel runner time on a pull request that does not touch `core/` | — |

## Consequences

### Positive

- `CodeQL` on a pull request compares every category of the base with a fresh
  analysis, so it succeeds or fails on findings, never on a skipped job.
- Every pull request gets C/C++ and Python code scanning, not only those whose
  diff the planner maps to the language.

### Negative

- A pull request that touches no C/C++ spends 17 to 26 more minutes of runner
  time on CodeQL C/C++. The job runs in parallel with the build matrix, so the
  time to green moves only when CodeQL C/C++ is the slowest job.

### Neutral / follow-ups

- `scripts/ci/test_security_workflow_contract.py` keeps the rule: a step-level
  `if:` or an impact-planner output in a CodeQL job fails it.

## References

- Controller order `vmafx-red-main-20261013`: fix the red master at the cause,
  not by loosening a check (paraphrased).
- [ADR-1140](1140-ci-impact-planner.md),
  [ADR-1222](1222-code-scanning-alert-triage-and-scope.md),
  [ADR-1389](1389-codeql-actions-universal-pr-sast.md),
  [ADR-3293](3293-aggregator-owed-contexts-must-succeed.md).
