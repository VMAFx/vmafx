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

GitHub-hosted Ubuntu runner images pre-provision
`/etc/apt/sources.list.d/*microsoft*` and `/etc/apt/sources.list.d/*azure*`
(`packages.microsoft.com`), which intermittently answer `403` or report that
the repository is no longer signed (the runner note now kept in
`scripts/ci/apt-install-cached.sh`) even when Ubuntu mirrors succeed; once
`--error-on=any` is active, those unremoved third-party lists abort the
refresh. Container builds also do not inherit host runner
`/etc/apt/apt.conf.d/80ci-retries` files.

An offline probe in `ubuntu:26.04@sha256:6a06111c3b0a` with `--network none` and
`apt 3.2.0` confirmed the exit behavior:

- `apt-get update` exits `0`.
- `apt-get update --error-on=any` exits `100`.
- `apt-get -o APT::Update::Error-Mode=any update` exits `100`.

## Decision

Pass `--error-on=any` and a positive `Acquire::Retries` option
(`-o Acquire::Retries=3`) on every `apt-get update` command across tracked CI,
shell and container sources (`ci_text_paths` in
`scripts/ci/test_fail_closed_ci.py`), including
`scripts/ci/apt-install-cached.sh` alongside its `apt.conf.d` configuration.
Standardize on lowercase `apt-get` (never bare `apt`) in scripts, workflows and
container files while preserving existing options (`-qq`, `-q`,
`-o Acquire::http::Timeout=30`), privilege prefixes (`sudo`, `$SUDO`),
environment assignments and `\` line continuations. Refuse any `-o` or
`--option` assignment to `APT::Update::Error-Mode` on refresh commands so a
trailing option cannot replace `--error-on=any`.

On GitHub-hosted runners (`.github/workflows/*.yml` and
`.github/actions/*/action.yml`), run every host APT refresh through
`scripts/ci/apt-install-cached.sh refresh` (when a step adds repositories or
runs its own `apt-get install`) or `scripts/ci/apt-install-cached.sh install`.
Before calling `sudo apt-get --error-on=any -o Acquire::Retries=3 update`,
`scripts/ci/apt-install-cached.sh` writes `/etc/apt/apt.conf.d/80ci-retries`
and removes `/etc/apt/sources.list.d/*microsoft*` and
`/etc/apt/sources.list.d/*azure*` so `packages.microsoft.com` outages cannot
break Ubuntu package refreshes. Container commands without `sudo` (such as
`docker run ... bash -c`) pass the inline flags directly. Propagate `sudo`
across nested shell command strings (`bash`, `sh`, `dash`, `zsh` and `ksh`
invoked via `sudo`, including `sudo` options and `env` prefixes) so hosted
workflows and composite actions cannot bypass the helper with `sudo sh -c` or
`sudo bash -c`, while container wrappers such as `sudo docker run ... bash -c`
remain permitted. Expand `env` split-string arguments (`-S`, clustered `-S`
such as `-iS`, and `--split-string`) in each shell clause with the same `shlex`
tokeniser before evaluating `apt` refreshes or `sudo` shell inheritance,
bounded to one pass per `env` token up to 8 expansions. Require hosted workflow
and composite action YAML files to parse cleanly and to write any multi-line
`run` scalar containing an APT refresh as a literal block (`run: |`) rather
than a folded, plain or quoted multi-line scalar. Reject duplicate scalar keys
within any YAML mapping in hosted workflows and composite actions so a later
`jobs`, `steps` or `run` key cannot overwrite an earlier entry.

Enforce these invariants through `AptUpdateFailClosedContract`
(`apt_update_problems` and `apt_update_contract_problems`) in
`scripts/ci/test_fail_closed_ci.py`, reusing `ci_text_paths` and the existing
`fail-closed-ci-contract` pre-commit hook, and widening both to include
`.Dockerfile` and `.Containerfile` suffixes.

## Alternatives considered

| Option | Benefit | Cost | Outcome |
| --- | --- | --- | --- |
| Inline `--error-on=any -o Acquire::Retries=3` on container and script `apt-get update` commands, `apt-install-cached.sh refresh` on hosted runners, and a shell-tokenised CI text contract | Self-contained in every container stage and script, centralizes runner Microsoft/Azure source removal on hosted workflows, and verifies shell command boundaries statically | Repeats two flags on container and script `apt-get update` commands and requires comments to avoid literal `apt-get ... update` phrasing | Chosen |
| Per-image `apt.conf.d` snippet | Centralizes configuration inside one container stage | Must be copied or written in every stage of every multi-stage `Dockerfile` and `Containerfile` plus host/VM scripts; cannot be proven by a stateless command check | Rejected |
| Outer shell retry loop | Wraps the command externally | Plain `apt-get update` exits `0` on fetch errors unless `--error-on=any` is already passed, and APT's built-in `Acquire::Retries=3` already retries per-URI transport failures without shell boilerplate | Rejected |
| Keep status quo | Zero edits to workflows, scripts or container files | Transient DNS, mirror or third-party runner source failures continue breaking refreshes or surfacing as misleading install-time missing-package errors | Rejected |

## Consequences

- The LLVM signing-key download retries eight times with constant 15-second
  delays (~2 min), because a runner can refuse connections beyond the old 7 s
  window; exhausted retries still fail closed (`scripts/dev/tidy-lane.sh`).
- Transient APT index download failures retry up to three times and fail
  immediately at `apt-get update` with exit `100` and the resolver or HTTP
  diagnostic, while hosted-runner workflows drop pre-installed Microsoft and
  Azure APT sources before refreshing.
- `AptUpdateFailClosedContract` scans `ci_text_paths(ROOT)` (tracked `.yml`,
  `.yaml`, `.sh`, `.bash`, `.Dockerfile` and `.Containerfile` suffixes plus
  basenames starting with `Dockerfile` or `Containerfile`, keeping paths
  without a dot suffix such as `docker/OtherDockerfile` excluded). Adding
  `.Dockerfile` and `.Containerfile` widens the shared `ci_text_paths` listing
  and `fail-closed-ci-contract` hook trigger, so the [ADR-3533][adr-3533]
  Launchpad PPA ban covers them too while retaining the 1,864-file bound.
  `_scan_ci_text` fails on any tracked in-scope path that is not a regular file
  (such as a symlink) without following or reading its target.
- `apt_update_problems` joins `\` line continuations (bounded to 200 physical
  lines), follows shell argument and operator boundaries (`;`, `&&`, `||`, `|`,
  `&`, `(`, `)`) via `shlex` tokenisation plus a bounded 8-item work list for
  nested `-c` command strings, fails closed on unbalanced quotes, matches exact
  `update` tokens (ignoring package or file names such as
  `update-notifier-common` or `update.sh`), requires `--error-on=any` and a
  positive final `Acquire::Retries` value across `-o` and `--option` flags, and
  rejects raw `sudo apt-get ... update` commands in `.github/workflows/*.yml`
  and `.github/actions/*/action.yml`.
- Bare `apt update`, mixed-case `APT-GET UPDATE`, and comments that write
  literal `apt-get ... update` text fail closed. Comments describing index
  refreshes avoid matching `\bapt(?:-get)?\b[^\n]*\bupdate\b`.
- Violations report `<path>:<line>: <message>` at the first physical line of
  the logical command (`<path>: <message>` for non-regular tracked paths),
  including:

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
