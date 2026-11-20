# ADR-3560: Refresh Python dependency locks on dependency-update branches

- **Status**: Accepted
- **Date**: 2026-11-18
- **Deciders**: Operator
- **Tags**: `ci`, `python`, `supply-chain`

## Context

The lock checker reads inputs, outputs and the pinned compiler version from
`requirements/locks/manifest.json`. A changed input leaves its derived locks
stale. The hosted dependency updater cannot run repository post-upgrade commands.

## Decision

The refresh workflow regenerates manifest outputs on dependency-update branches
and publishes them in a signed-off release-bot commit. A shared publisher also
serves the asset lock workflow. The DCO exception requires the exact refresh
subject, release-bot identity, dependency-bot opener, and only paths with live
entries in `.config/lint-exceptions.d/dco-python-lock-refresh.toml`. In landed
mode every changed path must have a live entry for the co-author strip to apply.
The 2026-11-18 operator answer `vmafx-renovate-python-lock-refresh` grants the
following 30 exact paths. A new lock output needs a new operator grant before an
entry is added.

- `requirements/locks/build.txt`
- `requirements/locks/tox.txt`
- `requirements/locks/nox.txt`
- `requirements/locks/pre-commit.txt`
- `requirements/locks/dev-linters.txt`
- `requirements/locks/cythonize.txt`
- `requirements/locks/mypy.txt`
- `requirements/locks/jsonschema.txt`
- `requirements/locks/docs-check.txt`
- `requirements/locks/windows-tester-zip.txt`
- `requirements/locks/package-build.txt`
- `requirements/locks/gcovr.txt`
- `requirements/locks/pytest-timeout.txt`
- `requirements/locks/tooling-tests.txt`
- `docs/requirements-lock.txt`
- `python/requirements-lock.txt`
- `python/requirements-test-lock.txt`
- `python/requirements-coverage-lock.txt`
- `ai/requirements-runtime-lock.txt`
- `ai/requirements-dev-lock.txt`
- `mcp-server/vmaf-mcp/requirements-runtime-lock.txt`
- `mcp-server/vmaf-mcp/requirements-production-lock.txt`
- `mcp-server/vmaf-mcp/requirements-dev-lock.txt`
- `dev/requirements-python-env-lock.txt`
- `tools/vmaf-tune/requirements-dev-lock.txt`
- `tools/rc1-tester/requirements-dev-lock.txt`
- `dev-llm/requirements-dev-lock.txt`
- `tools/vmaf-roi-score/requirements-dev-lock.txt`
- `tools/ensemble-training-kit/requirements-dev-lock.txt`
- `requirements/locks/twine.txt`

## Alternatives considered

| Alternative | Benefit | Cost |
| --- | --- | --- |
| Manual refresh | No workflow | Maintainers must repair each affected request. |
| Dependency updater owns compilation | No follow-up commit | Requires migration and verification of all lock inputs and outputs. |

## Consequences

Each exception uses Category D and lasts 120 days. An expired or absent entry
refuses that path. Remove an entry when the hosted limitation no longer applies;
renew it only while the limitation remains. A lease refuses publication if the
branch changes or is deleted during refresh. The next input change runs the
refresh again; otherwise rerun it against the current tip.

- `check` and `write` reject input pins for `uv` that differ from `uv_version`,
  so a Renovate PR that bumps an input pin fails until it also updates `uv_version`.

## References

- [ADR-3555](3555-intel-neo-lock-file.md): asset lock refresh and author handling.
- [Python dependency locks](../development/python-dependency-locks.md).
