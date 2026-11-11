# ADR-3537: Fail closed and retry on APT package index refreshes

- **Status**: Accepted
- **Date**: 2026-11-11
- **Tags**: ci, build, docker, tooling

## Context

Plain `apt-get update` treats repository index download failures as warnings and
exits `0` unless `--error-on=any` (or `APT::Update::Error-Mode=any`) is active.
When a transient DNS or mirror error prevents index retrieval, execution
continues into `apt-get install`, which then fails against stale or empty
package lists with an unrelated package-resolution message.

Two CI failures demonstrated both failure modes:

- In pull request 3403 (`Docker Image Build`), `Dockerfile:43` ran
  `scripts/ci/install-cuda-toolkit.sh`, whose `apt-get update -qq` (`:180`)
  emitted `W: Failed to fetch ... Could not resolve 'archive.ubuntu.com'` and
  exited `0`. The subsequent `apt-get install` failed with
  `E: Unable to locate package curl`, hiding the resolver failure.
- On 2026-11-11 in pull request 3413 (`5ce373ee4`, CI runs `43047738065` and
  `43053769874`), six `clang-tidy` container jobs ran `scripts/dev/tidy-lane.sh`
  (`:221`, `:237`, `:252`, `:269`, `:281`, `:311`). Its
  `apt-get -o Acquire::Retries=3 update -qq` emitted:

  ```text
  W: Failed to fetch http://archive.ubuntu.com/ubuntu/dists/resolute/InRelease  Could not resolve 'archive.ubuntu.com'
  W: Some index files failed to download. They have been ignored, or old ones used instead.
  ```

  It exited `0`, and `apt-get install` then failed with
  `E: Unable to satisfy dependencies` on `python3-yaml` (exit `100`). Bounded
  retries alone do not surface the index failure when every retry attempt fails.

Workflow runner steps write `Acquire::Retries "3"` to
`/etc/apt/apt.conf.d/80ci-retries` (such as `.github/workflows/build.yml:229`
and `scripts/ci/apt-install-cached.sh:22`), whereas container builds do not
inherit host runner APT configuration.

An offline probe in `ubuntu:26.04@sha256:6a06111c3b0a` with `--network none` and
`apt 3.2.0` confirmed the exit behavior:

- `apt-get update` exits `0`.
- `apt-get update --error-on=any` exits `100`.
- `apt-get -o APT::Update::Error-Mode=any update` exits `100`.

## Decision

Pass `--error-on=any` and `-o Acquire::Retries=3` on the same physical line as
every `apt-get` and `update` invocation across tracked CI, shell and container
sources (`ci_text_paths` in `scripts/ci/test_fail_closed_ci.py`), including
`scripts/ci/apt-install-cached.sh` alongside its `apt.conf.d` configuration.
Standardize on lowercase `apt-get` (never bare `apt`) in scripts, workflows and
container files while preserving existing options (`-qq`, `-q`,
`-o Acquire::http::Timeout=30`), privilege prefixes (`sudo`, `$SUDO`),
environment assignments and line continuations.

Enforce the invariant through `AptUpdateFailClosedContract`
(`apt_update_problems` and `apt_update_contract_problems`) in
`scripts/ci/test_fail_closed_ci.py`, reusing `ci_text_paths` and the existing
`fail-closed-ci-contract` pre-commit hook without YAML or shell parsing.

## Alternatives considered

| Option | Benefit | Cost | Outcome |
| --- | --- | --- | --- |
| Inline `--error-on=any -o Acquire::Retries=3` on every `apt-get update` line with a text contract | Self-contained at every call site across workflows, multi-stage container builds and scripts; verifiable line-by-line with zero parsing | Repeats two flags on each `apt-get update` line and requires comments to avoid literal `apt-get ... update` phrasing | Chosen |
| Per-image `apt.conf.d` snippet | Centralizes configuration inside one container stage | Must be copied or written in every stage of every multi-stage `Dockerfile` and `Containerfile` plus host/VM scripts; cannot be proven by a stateless single-line check | Rejected |
| Outer shell retry loop | Wraps the command externally | Plain `apt-get update` exits `0` on fetch errors unless `--error-on=any` is already passed, and APT's built-in `Acquire::Retries=3` already retries per-URI transport failures without shell boilerplate | Rejected |
| Keep status quo | Zero edits to workflows, scripts or container files | Transient DNS or mirror failures continue exiting `0` during index refresh and surface as misleading install-time missing-package errors | Rejected |

## Consequences

- Transient APT index download failures retry up to three times and fail
  immediately at `apt-get update` with exit `100` and the resolver or HTTP
  diagnostic, never proceeding to `apt-get install` with missing index lists.
- `AptUpdateFailClosedContract` scans `ci_text_paths(ROOT)` (tracked `.yml`,
  `.yaml`, `.sh` and `.bash` suffixes plus basenames starting with `Dockerfile`
  or `Containerfile`). Any physical line matching
  `\bapt(?:-get)?\b[^\n]*\bupdate\b` case-insensitively fails unless it is a
  non-comment lowercase `apt-get` call in which every `update` clause on the
  line carries both `--error-on=any` and `-o Acquire::Retries=<N>` (`N >= 1`).
- Bare `apt update`, mixed-case `APT-GET UPDATE`, and comments that write
  literal `apt-get ... update` text fail closed. Comments describing index
  refreshes avoid matching `\bapt(?:-get)?\b[^\n]*\bupdate\b`.
- Each violation reports:

  ```text
  <path>:<line>: apt-get update must fail on index errors: add --error-on=any -o Acquire::Retries=3 (ADR-3537)
  ```

- Positive, negative, boundary and planted-defect tests in
  `scripts/ci/test_fail_closed_ci.py` share isolated disposable Git repository
  setup with `LaunchpadPPAContract`.

## References

- [ADR-0106: Decision records](0106-adr-maintenance-rule.md).
- [ADR-3533: Install the tidy compiler from the Ubuntu archive][adr-3533]
  (`ci_text_paths` scope and `fail-closed-ci-contract` hook).
- APT `apt-get(8)` manual page: `--error-on=any` (`APT::Update::Error-Mode`) and
  `Acquire::Retries`.
- [CI run 43047738065][ci-run-43047738065] and
  [CI run 43053769874][ci-run-43053769874], 2026-11-11, plus pull request 3403
  container build failure in `scripts/ci/install-cuda-toolkit.sh:180`.
- Source: `req` (operator order `finding-repo-health-999f71af7d-20261111-0601`,
  2026-11-11, paraphrased): repair red checks and add a defence for each failure
  class.

[adr-3533]: 3533-tidy-gcc-from-ubuntu-archive.md
[ci-run-43047738065]: https://github.com/VMAFx/vmafx/actions/runs/43047738065
[ci-run-43053769874]: https://github.com/VMAFx/vmafx/actions/runs/43053769874
