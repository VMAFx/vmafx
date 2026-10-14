<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3312: One Renovate manager owns the CI Python interpreter

- **Status**: Accepted
- **Date**: 2026-10-14
- **Deciders**: Lusoris (operator decision vmafx-2744-python-315)
- **Tags**: ci, renovate, python, supply-chain

## Context

CI installs one Python interpreter, named twice: `PYTHON_CI_VERSION` in
`build-config.env` and the `python-version:` input of every
`actions/setup-python` step under `.github/workflows/`.
`scripts/ci/check-workflow-versions.py` refuses a workflow pin that differs
from `build-config.env`, so the two must move in one commit.

Renovate's `github-actions` manager extracts each `python-version:` input as
dependency `python` (datasource `github-releases`, package
`actions/python-versions`) and raises it on its own. No manager read
`PYTHON_CI_VERSION`. A Python release therefore produced a pull request that
changed the workflows and left `build-config.env` behind, which the
single-source gate refuses by construction. Pull request #2744 (3.14.8 to
3.15.0) is such a pull request. Python 3.15 is also a minor-version change
inside the 1.0.0 scope freeze.

## Decision

- A regex custom manager in `renovate.json` reads `PYTHON_CI_VERSION` from
  `build-config.env` and every `python-version:` pin of a workflow file, as one
  dependency `python` from `actions/python-versions` with the datasource,
  versioning (`npm`) and `extractVersion` the `github-actions` manager uses for
  setup-python. Renovate groups a dependency's files into one branch, so the
  interpreter moves in one pull request.
- A package rule disables the `github-actions` manager's `python`
  (`uses-with`) dependency, so only the custom manager raises those pins.
- A package rule names the group `CI Python interpreter`, keeps updates below
  `3.15.0` until the 1.0.0 freeze decision is revisited, and auto-merges
  patch updates under the normal required checks.
- `scripts/ci/tests/test_renovate_file_patterns.py` holds the manager to the
  pins the single-source gate reads.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Custom manager owns both files; github-actions copy disabled (chosen) | One pull request per interpreter release; the gate's two sides move together | One more regex manager to keep in step with the gate | — |
| Keep the github-actions manager and add a custom manager for `build-config.env` only | Smaller change | Two managers, two branches for one version; the first to land fails the gate | Splits one dependency across two pull requests |
| Make `build-config.env` the only pin and have workflows read it at run time | One literal | `setup-python` takes the version as an input; every workflow needs a read step before it, and the pin leaves Renovate's view of the workflow | More moving parts in 14 workflows than the drift it removes |
| Merge #2744 by hand with `build-config.env` added | Takes 3.15 now | Python 3.15 is a minor change inside the 1.0.0 freeze; the next release repeats the split | The freeze defers 3.15 (paraphrased operator decision) |

## Consequences

- The next Python patch release opens one pull request titled after the
  `CI Python interpreter` group that changes `build-config.env` and the
  workflow pins together, and passes `check-workflow-versions.py`.
- Pull request #2744 is superseded; Renovate closes or rebases it once its
  dependency is disabled for the `github-actions` manager.
- Raising the interpreter to 3.15 is a later decision: lift the
  `allowedVersions` bound in the same change that raises mypy's
  `python_version` and `requires-python` (ADR-1282).

## Supply-chain impact

None on shipped artifacts. The interpreter source and versioning are
unchanged; only the manager that opens the update changes.

### SBOM delta

None.

### Carbon / footprint

One Renovate pull request per interpreter release instead of one that fails
its checks and one follow-up.

## References

- Operator decision `vmafx-2744-python-315`, 2026-10-13 (paraphrased): keep
  #2744 open, add a custom manager that owns `PYTHON_CI_VERSION` and the
  workflow pins as one group, stop the github-actions manager touching them,
  and defer Python 3.15 past the 1.0.0 freeze.
- [ADR-0363](0363-renovate-replaces-dependabot.md): Renovate as the update bot.
- [ADR-1282](1282-mypy-python-version-follows-requires-python.md): mypy's
  `python_version` follows `PYTHON_CI_VERSION`.
- `scripts/ci/check-workflow-versions.py`: the single-source gate.
