# ADR-3533: Install the tidy compiler from the Ubuntu archive

- **Status**: Accepted
- **Supersedes**: [ADR-1230](1230-modern-gcc-toolchain.md), decision 1 only
- **Date**: 2026-11-11
- **Tags**: ci, build, tooling, clang-tidy

## Context

The three tidy jobs run on `ubuntu-26.04` (resolute). Its archive supplies
`gcc-15` and `g++-15` at `15.2.0-16ubuntu1`. Both committed tidy baselines
record `gcc-15 (Ubuntu 15.2.0-16ubuntu1) 15.2.0`, so the existing apt package
lists already select the compiler used for those measurements.

Registering `ppa:ubuntu-toolchain-r/test` supplied no newer compiler and
introduced a live Launchpad API dependency. That registration failed CI
twice: the earlier failure associated with pull request 3270 returned HTTP
500 with `GPGKeyTemporarilyNotFoundError`; the 2026-11-11 Tidy Ratchet failure
ended in a `login_anonymously` traceback during dependency installation.

## Decision

Install `gcc-15` and `g++-15` from the Ubuntu 26.04 archive through the
existing apt package lists. Remove PPA registration from all three tidy
jobs. If a compiler newer than the archive is needed, introduce a pinned
source with an explicit version and integrity verification, never a live
API lookup. This supersedes only decision 1 of ADR-1230; compiler-version
recording and diagnostic reports remain as specified in decisions 2 and 3.

## Alternatives considered

| Option | Benefit | Cost | Outcome |
| --- | --- | --- | --- |
| Keep registering the PPA | Retains the earlier source choice | Adds a failing API dependency without a newer measured compiler | Rejected |
| Use the Ubuntu archive | Supplies the recorded compiler through existing package lists | A newer compiler requires a separately pinned source | Chosen |
| Retry the Launchpad API | May recover a transient failure | Retains the unnecessary dependency and cannot guarantee recovery | Rejected |

## Consequences

- Dependency installation no longer calls the Launchpad API to register a PPA.
- The recorded compiler version stays unchanged; this change does not
  regenerate baselines.
- The fail-closed CI contract bans source-registration text, case-insensitively:
  `add-apt-repository`, `apt-add-repository`, a word boundary followed by
  `ppa:`, and the `ppa.launchpad.net` and `ppa.launchpadcontent.net` hosts.
  Each matching line reports its file, line number and matched text.
- Scope includes every tracked `.yml`, `.yaml`, `.sh` and `.bash` file, plus
  every basename beginning with `Dockerfile` or `Containerfile`, throughout
  the repository, including documentation examples. Local hook triggers cover
  the same paths. The 1,864-file bound allows fourfold growth from 466 tracked
  source files measured on 2026-11-11; exceeding it fails the contract.
- Comments, output strings, heredoc data and variable assignments count as
  matches. No YAML or shell parsing occurs. Use ADR-3533 wording without
  banned text when an example or comment needs to describe this restriction.
  Listed non-regular paths are skipped with an explicit diagnostic; source
  reads replace invalid UTF-8 bytes. Failed Git listings raise errors.
- Positive, negative and boundary cases cover nested shell commands, aliases,
  merge keys, relocated actions, tracked-file scope and exact source locations.

## References

- [ADR-0106: Decision records](0106-adr-maintenance-rule.md).
- [ADR-1230: Compiler and diagnostics](1230-modern-gcc-toolchain.md),
  decision 1 superseded here.
- [Ubuntu resolute gcc-15 package][gcc15], observed 2026-11-11 at 12:42 UTC:
  `15.2.0-16ubuntu1`.
- [CI run 43017193022][ci-run], Tidy Ratchet dependency-install failure,
  2026-11-11 at 12:06 UTC; earlier matching failure associated with pull
  request 3270.
- Baseline evidence: `scripts/ci/tidy-baseline-cpu.json` and
  `scripts/ci/tidy-baseline-cuda.json`, field `cc_version`, inspected at
  commit `6f42cb9fa`.
- Source: `req` (operator order `finding-repo-health-999f71af7d-20261111-0601`,
  2026-11-11, paraphrased): repair the red default branch, whose Tidy Ratchet
  failed while registering the toolchain PPA through Launchpad.

[gcc15]: https://packages.ubuntu.com/resolute/gcc-15
[ci-run]: https://github.com/VMAFx/vmafx/actions/runs/43017193022
