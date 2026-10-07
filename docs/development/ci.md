# CI overview

Run the local gate before you push, then let CI repeat it. This page tells a
contributor which checks exist, which of them block a merge, and where to find
the detail. The authoritative trigger and gate behaviour lives in the workflow
files under [`.github/workflows/`](../../.github/workflows/); the topic pages
linked below carry the long procedures.

## Before you push

Run the local subset of CI. It catches the usual formatter, lint and fast-test
failures in seconds, against a 10-minute CI round trip:

```bash
make verify-all     # HISS/context/evidence, duplicate implementations, docs gate
make dedupe-check   # fast standalone AST clone scan
make format-check   # clang-format + black + ruff, no writes
make lint           # configured native + Python, shell, Markdown, Go and docs checks
make test-fast
bash scripts/ci/twin-drift-check.sh  # .c/.cpp twin drift + stale source refs (ADR-1135)
pre-commit run --all-files  # if .pre-commit-config.yaml hooks are installed
```

The format-check and pre-commit pair catches roughly the same surface as the
`pre-commit` job of `lint-and-format.yml`.

The duplicate scan is deliberately separate from `standardsctl audit`: the audit
baseline does not include AST clones. `make verify-all`, pre-commit, pre-push
and the required Standards job therefore invoke `standardsctl dedupe scan .`
explicitly. A finding is a hard failure; there is no origin, generated-code or
historical-debt exemption.

| Topic | Page |
| --- | --- |
| Configure a lint build, read receipts, cppcheck models | [Local lint](local-lint.md) |
| Whole-tree clang-tidy baselines and lanes | [Tidy ratchet](tidy-ratchet.md), [measuring the lanes](tidy-lanes.md) |
| `docs/state.md` gates and rebase conflicts | [state.md gates](state-md-gates.md) |
| Praetor documentation gate, text register, pin moves | [Praetor gate](praetor-gate.md) |
| Display names of every check | [CI job display names](ci-job-names.md) |

## Workflows

The files under `.github/workflows/` and what each is for. Which checks block a
merge is decided by the [aggregator](#required-checks-aggregator); for the
mapping of shortened names and conventions see
[CI job display names](ci-job-names.md), and for every build lane, whether it is
required and which ADR owns it, see
[ADR-1259](../adr/1259-ci-build-matrix-as-it-runs.md).

### Pull-request checks

| File | Purpose |
| --- | --- |
| [`required-aggregator.yml`](../../.github/workflows/required-aggregator.yml) | Single required-check aggregator (ADR-0313). |
| [`lint-and-format.yml`](../../.github/workflows/lint-and-format.yml) | Pre-commit, clang-tidy (changed files plus the whole-tree ratchet, ADR-1142), cppcheck, mypy, registry validate, twin-drift gate (ADR-1135). |
| [`standards-gate.yml`](../../.github/workflows/standards-gate.yml) | Required HISS and context verification and the fail-closed duplicate-implementation scan. |
| [`rule-enforcement.yml`](../../.github/workflows/rule-enforcement.yml) | ADR-0100, 0106, 0108 and 0165 process gates. |
| [`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml) | Netflix golden, sanitizers, tiny-AI, MCP, coverage, assertion density, the test-suite registry and tooling suite (`Tooling Tests`) and the Python package suites (`Python Package Tests (<suite>)`); which job runs which test suite: [test suites](test-suites.md). |
| [`security-scans.yml`](../../.github/workflows/security-scans.yml) | Semgrep, CodeQL, Gitleaks, Dependency Review. |
| [`libvmaf-build-matrix.yml`](../../.github/workflows/libvmaf-build-matrix.yml) | Cross-platform, cross-backend libvmaf build matrix: 17 lanes, six of them required. |
| [`build.yml`](../../.github/workflows/build.yml) | One all-backend build per OS (`Linux Intel LLVM`, `macOS Clang+Metal`, `Windows MSVC+CUDA (full)`), alongside the matrix; required since ADR-1297. |
| [`go-ci.yml`](../../.github/workflows/go-ci.yml) | Required Go modernization, vet, security scan, runner smoke and tests (ADRs 1238 and 1338). |
| [`ffmpeg-integration.yml`](../../.github/workflows/ffmpeg-integration.yml) | FFmpeg plus libvmaf build (Linux GCC, macOS Clang, SYCL). |
| [`ffmpeg-patch-stack.yml`](../../.github/workflows/ffmpeg-patch-stack.yml) | Replays the cumulative FFmpeg patch series (see [FFmpeg patch automation](ffmpeg-patch-automation.md)). |
| [`sycl-parity.yml`](../../.github/workflows/sycl-parity.yml) | SYCL parity on the self-hosted Intel Arc A380 runner (ADR-1177; see the [runbook](ci-self-hosted-sycl.md)). |
| [`docs.yml`](../../.github/workflows/docs.yml) | Docs build. |
| [`doxygen-public-api.yml`](../../.github/workflows/doxygen-public-api.yml) | Doxygen build of the public C API; required since ADR-1297. |
| [`docker-image.yml`](../../.github/workflows/docker-image.yml) | Docker image build. |
| [`release-dry-run.yml`](../../.github/workflows/release-dry-run.yml) | Builds the release images and the `vmaf-mcp` distribution and SBOM without publishing (ADR-1595); required context `Release Dry Run` on pull requests ([ADR-1687](../adr/1687-required-release-dry-run-legs.md)). |
| [`windows-tester-bundle.yml`](../../.github/workflows/windows-tester-bundle.yml) | The Windows tester zips: the x64 zip on a pull request, all four on a push, published on dispatch; required context `Windows Tester Zip` (ADR-1687). |
| [`dev-container-build.yml`](../../.github/workflows/dev-container-build.yml) | PR-time build gate for `dev/Containerfile` (ADR-0819). |
| [`helm-chart.yml`](../../.github/workflows/helm-chart.yml) | `helm lint` of the chart. |
| [`rust-ci.yml`](../../.github/workflows/rust-ci.yml) | Rust crates: `cargo fmt --all` and `clippy --workspace`, `cargo test --workspace`, the golden smoke example and `cargo-deny`; the planner may skip the work, the gates `vmafx-sys CI` and `cargo-deny` are required. |
| [`sanitizers.yml`](../../.github/workflows/sanitizers.yml) | Combined ASan and UBSan on PRs, TSan on master pushes, nightly fuzzing; not required (the required sanitizers are in `tests-and-quality-gates.yml`). |
| [`praetor-docs.yml`](../../.github/workflows/praetor-docs.yml) | Praetor's Documentation Governance gate for the `docs:seo-portal` facet; praetor-managed, not required. See [Praetor gate](praetor-gate.md). |
| [`praetor-api.yml`](../../.github/workflows/praetor-api.yml) | Praetor's `Go API Compatibility` gate (`go-apidiff` over every Go module; no path filter). Praetor-managed; required through the aggregator (ADR-1506), its marker sits in `standards-gate.yml`. |
| [`scorecard-policy.yml`](../../.github/workflows/scorecard-policy.yml) | OpenSSF Scorecard PR policy (ADR-1247). |
| [`pr-type-label.yml`](../../.github/workflows/pr-type-label.yml) | Derives a `type:*` label from the Conventional-Commit prefix of the PR. |

### Scheduled, release and watcher workflows

| File | Purpose |
| --- | --- |
| [`nightly.yml`](../../.github/workflows/nightly.yml) | Nightly jobs, including the whole-tree clang-tidy ratchet of the `cpu` lane. |
| [`nightly-bisect.yml`](../../.github/workflows/nightly-bisect.yml) | Nightly bisect-model-quality smoke against a committed fixture timeline. |
| [`fuzz.yml`](../../.github/workflows/fuzz.yml) | Nightly libFuzzer smoke over every harness under `core/test/fuzz/`. |
| [`scorecard.yml`](../../.github/workflows/scorecard.yml) | Weekly OpenSSF Scorecard scan. |
| [`e2e-k8s.yml`](../../.github/workflows/e2e-k8s.yml) | Kubernetes integration harness for the VMAFx runtime; label- and schedule-gated. |
| [`release-please.yml`](../../.github/workflows/release-please.yml) | On each push to `master`: opens or updates the release PR, or creates the draft release (ADR-1127, ADR-1128). |
| [`supply-chain.yml`](../../.github/workflows/supply-chain.yml) | Build provenance, Sigstore signing and SBOM when a release draft is published. |
| [`dev-container-publish.yml`](../../.github/workflows/dev-container-publish.yml) | Builds, pushes and signs the canonical dev container image to GHCR, only into a private package ([ADR-1564](../adr/1564-dev-image-private-guard.md)). |
| [`docker-publish-production.yml`](../../.github/workflows/docker-publish-production.yml) | Builds, pushes, signs and SBOMs the production image on release publication. |
| [`docker-publish-operator-node.yml`](../../.github/workflows/docker-publish-operator-node.yml) | The same for the VMAFX Go service images. |
| [`published-rc-licence-companions.yml`](../../.github/workflows/published-rc-licence-companions.yml) | Manual: notices, `<tag>-source` companions and SBOMs for the images and release files published for 1.0.0-rc.1 and rc.2 ([ADR-1578](../adr/1578-published-rc-licence-companions.md)). |
| [`docker-publish-tester.yml`](../../.github/workflows/docker-publish-tester.yml) | Builds, tests, signs and attests the tester image; its pull-request run (the amd64 image) is the required context `Tester Image` (ADR-1687). |
| [`macos-tester-bundle.yml`](../../.github/workflows/macos-tester-bundle.yml) | Builds, tests, attests and publishes the macOS arm64 tester bundle. |
| [`upstream-watcher.yml`](../../.github/workflows/upstream-watcher.yml) | Polls FFmpeg master for upstream-blocked features ([upstream watchers](upstream-watchers.md)). |
| [`upstream-ffmpeg-hip-hwdec-watcher.yml`](../../.github/workflows/upstream-ffmpeg-hip-hwdec-watcher.yml) | Weekly watch for an FFmpeg ROCm/HIP hwdec context type (ADR-0448). |
| [`upstream-netflix-645-hdr-model-watcher.yml`](../../.github/workflows/upstream-netflix-645-hdr-model-watcher.yml) | Watches Netflix/vmaf#645 and the upstream HDR model (ADR-0448). |
| [`upstream-netflix-955-watcher.yml`](../../.github/workflows/upstream-netflix-955-watcher.yml) | Watches Netflix/vmaf#1494, the upstream fix for #955 (ADR-0448). |

## Python type-check gate

`Python Lint` is a required, fail-closed check. It installs the reviewed
`requirements/locks/mypy.txt` lock and runs the same merge-base gate as the
local `mypy-local` pre-push hook:

```bash
python3 scripts/git-hooks/pre-push-mypy.py
```

The gate checks added, copied, modified, renamed and type-changed tracked
`*.py` paths under `ai/` and `scripts/`. It reports only findings absent from
the selected merge base, but an analysis crash, a missing tool, base or file, or
an unattributable nonzero status fails the job. `ai/src/` is checked separately
with `--explicit-package-bases` so each module has one canonical identity.

Pull requests compare with `origin/master`. A master push compares with the
event's previous commit, so hosted post-merge validation covers the pushed
range. See [ADR-1310](../adr/1310-mypy-ci-fail-closed.md) and the detailed
[hook contract](pre-commit-hooks.md#python-push-scope).

## Draft pull requests defer heavy CI

A draft PR cannot satisfy the required aggregator. Per
[ADR-0331](../adr/0331-skip-ci-on-draft-prs.md), every `pull_request`-triggered
workflow is gated to skip while the PR is a draft, except the aggregator, which
explicitly fails drafts:

- each workflow's `pull_request:` block lists
  `types: [opened, synchronize, reopened, ready_for_review]`;
- each top-level job carries an `if:` clause of the form
  `github.event_name != 'pull_request' || github.event.pull_request.draft == false`.

What this means for contributors:

1. **A draft PR cannot satisfy the required aggregator.** The aggregator starts
   and fails with a request to mark the PR ready. Heavy jobs remain skipped
   until ready-for-review, so skipped draft-era checks are never mistaken for
   completed validation.
2. **Promoting the draft to ready-for-review fires CI exactly once.** GitHub's
   `ready_for_review` event re-triggers the workflows; later `synchronize`
   events on the now-ready PR fire CI as before.
3. **Pushing to `master` is unaffected.** The job-level `if:` clause
   short-circuits to `true` when there is no PR object (for example on `push:`
   events).

To preview CI status before merging, mark the PR ready-for-review. You can flip
back to draft afterwards; the next `ready_for_review` fires a fresh matrix.

## CI impact routing (ADR-1140)

Required checks do not decide whether they apply from a workflow-level `paths:`
or `paths-ignore:` filter. Every workflow that hosts a check named in
`required-aggregator.yml` starts on every non-draft PR and every push to
`master`; the first step of each required job runs the planner:

```bash
python3 scripts/ci/plan-ci-impact.py --event pull_request \
  --base <base-sha> --head <head-sha> --github-output "$GITHUB_OUTPUT"
```

The planner diffs the event's exact revisions (the merge-base of head and base
for a PR, the exact `before..head` for a push) and maps the changed paths onto
the selectors declared in `.github/ci-impact.json`. There are 19 selectors:

| Selector | Owns | Gates |
| --- | --- | --- |
| `c_core` | `core/`, `ffmpeg-patches/`, `model/`, `testdata/`, golden fixtures | Build legs, sanitizers, cppcheck, CodeQL C/C++, assertion density, Tidy Ratchet |
| `python` | `python/`, `compat/`, `mcp-server/`, `tools/`, `dev-llm/`, `scripts/**/*.py`, `requirements/` | Inherited by the composite selectors below |
| `ai` | `ai/`, `model/` | Inherited by `tiny_ai` and `python_lint` |
| `go` | `cmd/`, `pkg/`, `internal/`, `api/`, `proto/`, `gen/`, `*.go`, `go.mod` | Inherited by `go_checks` |
| `golden_harness` | `c_core` plus `python` | Netflix golden tests, coverage gate |
| `tiny_ai` | `c_core` plus `ai` plus `python` | Tiny AI (DNN suite and `ai/` pytests) |
| `python_lint` | `python` plus `ai` | CodeQL Python |
| `go_checks` | `go` plus `c_core` | Go vet, security scan, native and ORT smoke, and Go tests |
| `docs` | `docs/`, `mkdocs.yml`, `*.md`, `changelog.d/` | Docs build |
| `actions` | `.github/` | CodeQL Actions, FFmpeg patch stack |
| `docker_image` | `c_core` plus the `Dockerfile` and Python requirements | Docker image build |
| `dev_container` | `c_core`, `python`, `ai`, `go`, `shell` plus `dev/` | Dev container build |
| `doxygen` | `core/include/libvmaf/`, the public-API Doxyfile | Doxygen public API |
| `helm` | `deploy/helm/` | Helm chart |
| `tester_image` | `docker/Dockerfile.tester`, `tools/rc1-tester/`, the toolkit install scripts, `build-config.env`, the licence inputs; own paths only ([ADR-1700](../adr/1700-tester-selectors-own-paths-only.md)) | `Tester Image` ([ADR-1687](../adr/1687-required-release-dry-run-legs.md)) |
| `windows_tester_zip` | The Windows zip build scripts, its lock file and the Windows inputs under `tools/rc1-tester/image/`; own paths only ([ADR-1700](../adr/1700-tester-selectors-own-paths-only.md)) | `Windows Tester Zip` ([ADR-1687](../adr/1687-required-release-dry-run-legs.md)) |
| `rust` | `bindings/`, `Cargo.*`, `core/src/feature/rust/` | Rust CI (path-filtered, not required) |
| `shell` | `*.sh` | Not required, still path-filtered |
| `container` | `Dockerfile*`, `dev/`, `docker/`, `deploy/`, `.devcontainer/` | Not required, still path-filtered |

Steps gated on a selector that is not impacted are skipped and the job emits
`::notice::<selector> not impacted (mode=... reason=...)` before reporting
`success`, so the aggregator always sees a real conclusion with a real reason.

The planner fails closed. Each of these produces `mode=full`, which sets every
selector true (the behaviour before ADR-1140):

- an unknown top-level path;
- any status other than add or modify (delete, rename, copy, mode change);
- a change to a CI-authority file (the map, the planner, `scripts/ci/**`, the
  workflows hosting required contexts, `.pre-commit-config.yaml`, `Makefile`,
  `.clang-tidy` and the like);
- a missing merge-base, a non-linear push or an over-large diff.

One declared exception ([ADR-1700](../adr/1700-tester-selectors-own-paths-only.md)):
a selector with `"own_paths_only": true` is not set by the fallback itself. When
the changed paths are known, it is true only if one of them matches its own
patterns; when they are not (a dispatch, a schedule, a diff that could not be
read), it stays true. Only `tester_image` and `windows_tester_zip` carry it, it
needs patterns and no `inherits`, and `test_ci_impact.py` fails on any other use.
A change under `scripts/ci/` alone therefore runs every other gate and neither
tester build.

Run it locally:

```bash
python3 scripts/ci/plan-ci-impact.py --event pull_request \
  --base "$(git merge-base origin/master HEAD)" --head HEAD --print
python3 -m unittest scripts/ci/tests/test_ci_impact.py   # map and tree contract
```

Adding a top-level directory or file? Add it to `known_prefixes` or
`known_files` (and to a selector if a required check owns it); the contract test
fails otherwise, because an unknown path would silently force `full` mode on
every PR that touches it.

## Master push runs

Master commits land every few minutes through a local merge train. A push to
`master` must not cancel the runs of the previous master commit, or no master
commit ever gets a complete verdict ([ADR-1673](../adr/1673-master-runs-not-cancelled-by-concurrency.md)).

- Every workflow triggered by a push to `master` puts the SHA in its group on
  master and keeps the ref elsewhere:

  ```yaml
  group: <name>-${{ github.ref == 'refs/heads/master' && github.sha || github.ref }}
  cancel-in-progress: ${{ github.ref != 'refs/heads/master' }}
  ```

  On a PR ref the group is still the ref and a newer push cancels the older
  run; on master each commit has its own group and nothing cancels it.
- The merge train keeps one sentinel master commit's runs alive and cancels the
  other superseded master runs itself through the API. GitHub's per-ref
  cancellation is not involved on master.
- The Required Checks Aggregator follows the same rule. Re-running the
  aggregator of an old master head no longer cancels the newest one. On a PR ref
  the old warning stands: re-running an older head's aggregator cancels the
  current head's.
- Blocks that must serialise keep their group and are listed with a reason in
  `scripts/ci/tests/test_master_concurrency_contract.py`: `dev-container-publish.yml`
  (registry publish), `release-please.yml` (one release PR per push),
  `scorecard.yml` (attestation publish) and the `deploy` job of `docs.yml` (one
  Pages deployment).
- A new workflow that triggers on a push to `master` takes the SHA form or is
  added to that list with a reason. Run the check with
  `python3 -B -m unittest scripts/ci/tests/test_master_concurrency_contract.py`.

## Depot runners

Four long Linux jobs of a master push can run on
[Depot](https://depot.dev/docs/github-actions/runner-types) runners instead of
GitHub-hosted ones ([ADR-2168](../adr/2168-depot-runners.md)). The arrangement
is temporary, until another CI provider is chosen.

| Job | Workflow | Median on GitHub-hosted |
| --- | --- | --- |
| `Coverage Gate` | `tests-and-quality-gates.yml` | 52 min |
| `Dev Container Build work` | `dev-container-build.yml` | 49 min |
| `Docker Image Build work` | `docker-image.yml` | 29 min |
| `FFmpeg SYCL work` | `ffmpeg-integration.yml` | 27 min |

- **Switch on**: set the repository variable `VMAFX_DEPOT_LINUX_RUNNER` to a
  Depot label, for example
  `gh variable set VMAFX_DEPOT_LINUX_RUNNER --body depot-ubuntu-24.04-8`. The
  label's suffix is the vCPU count, and a base minute is two vCPU-minutes. The
  Depot organisation must be connected to the repository first.
- **Switch off**: `gh variable delete VMAFX_DEPOT_LINUX_RUNNER`. Unset or empty
  means GitHub-hosted, and no commit is needed.
- **Scope**: only a push or dispatch on `master` uses the variable. Pull
  requests, fork pull requests, other branches and every other job stay on
  GitHub-hosted runners.
- **Usage**: `python3 scripts/ci/depot_minutes.py` prints this month's base
  minutes against the Depot organisation's limit of 10,000 and exits 1 at
  90 %, so a caller can clear the variable. It reads the Actions API only
  (`gh` must be logged in).
- Depot images are Ubuntu 24.04 and 22.04; the four jobs ask for
  `ubuntu-latest`.

## Required-checks aggregator

The single required check on `master` branch protection is the **Required
Checks Aggregator** ([ADR-0313](../adr/0313-ci-required-checks-aggregator.md)).
The `required` array in `required-aggregator.yml` is the merge gate in full: a
check missing from it can be red while the merge button stays green
([ADR-1297](../adr/1297-ci-gate-every-reporting-check.md)).

It runs on every PR and master push. Draft PRs fail immediately; ready PRs poll
for the named sibling check runs to reach a terminal state and accept
`success`, `skipped` or `neutral` per check. Results predating the current run
are excluded, so skipped draft-era checks cannot mask ready validation.

On a master push the aggregator reads only the push's own check runs: a check
run whose check suite belongs to a pull-request workflow run on the same commit
is left out. The merge train lands by fast-forward, so a landed commit is also
the head of its pull request, and the train cancels that pull request's runs
once it lands; without the filter those cancelled runs of the pull-request-only
gates (`Deliverables Checklist`, `docs/state.md Gate`, `Silent-Revert Guard`
and others) read as failures of every master push. A check run of a workflow
run on another branch is left out too: release-please opens its release-notes
branch at the master head, and the push runs there of the workflows without a
branch filter are cancelled as superseded. Check runs of other apps, such as
code scanning, always count. A pull-request aggregator reads every check run on
its head as before. `scripts/ci/tests/test_aggregator_event_scope.py` runs the
embedded script on both events.

### Release legs

Three contexts gate what the release and tester workflows build on a pull
request ([ADR-1687](../adr/1687-required-release-dry-run-legs.md)). All three
must report `success`; a missing or skipped one fails the aggregator.

| Context | Workflow | Does work when | Required on |
| --- | --- | --- | --- |
| `Tester Image` | `docker-publish-tester.yml` | the planner selects `tester_image` | pull requests and master pushes |
| `Windows Tester Zip` | `windows-tester-bundle.yml` | the planner selects `windows_tester_zip` | pull requests and master pushes |
| `Release Dry Run` | `release-dry-run.yml` | always; `scripts/ci/release-dry-run-plan.sh` picks the groups | pull requests only (the workflow has no push trigger; the aggregator list `pullRequestOnly` drops it from other runs) |

An unselected tester run passes in about a minute. Which inputs select them and
how to reproduce a failure: [verifying the release and tester workflows](release-workflow-verification.md).

On a push to `master` the two tester gates need the whole push run of their
chain, so when the push touches their inputs the master aggregator waits for both
image architectures and all four Windows zips, and a red push leg turns it red.
That is intended (ADR-1687, confirmed in ADR-1700): a broken tester package on
`master` is visible in the same place as every other red check.

### Go checks

The `go vet + go test` job is required under
[ADR-1238](../adr/1238-go-security-required-gate.md). Its native build,
security scan, runner smoke and tests run for Go or core and model inputs;
unrelated documentation changes report success after an explicit impact notice.
A failing `gosec` scan blocks merging even though it prevents later Go tests
from running. The scan is `make lint-go`, the target `make lint` runs, so run
it before pushing a Go change; its flags and the reason for each sit next to
the target in the `Makefile`. The job also starts on ready-for-review events,
so draft-era results cannot replace the current validation run.

Before installing native build dependencies, the job runs `go fix -diff ./...`
under the exact `go.mod` toolchain. Per
[ADR-1338](../adr/1338-go-fix-clean-tree-gate.md), any available source rewrite
is a blocking failure; CI never mutates the checkout. Run `make go-fix`, repeat
if the Go tool reports cascading fixes, and finish with `make go-fix-check`
locally.

### Hardware-dependent lanes

Two checks depend on self-hosted hardware. Both fail closed when their lane is
enabled and accept a skip only while it is disabled.

| Check | Switch | While disabled | While enabled | Runbook |
| --- | --- | --- | --- | --- |
| `SYCL Parity (Arc A380)` ([ADR-1177](../adr/1177-sycl-arc-self-hosted-runner.md)) | repository variable `SYCL_ARC_RUNNER_ENABLED` | An absent or skipped job is accepted. | The job must report `success`; a skip, which is what the loud probe failure in `sycl-parity.yml` produces when the runner is unregistered, offline or the probe token is rejected, fails the aggregator. | [ci-self-hosted-sycl.md](ci-self-hosted-sycl.md) |
| `Coverage GPU` ([ADR-1319](../adr/1319-fail-closed-self-hosted-gpu-admission.md)) | `GPU_COVERAGE_ENABLED` plus the complete `self-hosted,linux,gpu-full` online label set | Absence or skip is permitted. | `Coverage GPU` must report success; probe failure, absence, skip or neutral blocks the aggregator. | [self-hosted-runner.md](self-hosted-runner.md) |

`Coverage GPU` is a distinct `gpu-full` capability: a hosted job checks the
variable and the label set before the hardware job can be dispatched. The
Arc-only runner must not be relabelled to satisfy this CUDA plus SYCL contract.

## Twin-drift gate

`core/` carries same-directory `.c`/`.cpp` twin pairs left by the C++23
migration ([ADR-0729](../adr/0729-cpp23-wave3-bundle.md)). Twice a fix landed
on one twin and never reached the other, and twice a rename (`mem.c` to
`mem.cpp`, `dict.c` to `dict.cpp`) left a stale path in a build file that only
nightly or opt-in lanes configure.
[ADR-1135](../adr/1135-ci-twin-drift-gate.md) turns both into a blocking,
required check: `Twin Drift` (job `twin-drift-check`) in
[`lint-and-format.yml`](../../.github/workflows/lint-and-format.yml), backed by
[`scripts/ci/twin-drift-check.sh`](../../scripts/ci/twin-drift-check.sh).

### What fails

The gate fails when either holds:

1. A same-directory `.c`/`.cpp` pair exists and one side is compiled by no build
   file (`meson.build`, `setup.py`, `*.pyx`), unless that side is listed in
   [`scripts/ci/twin-drift-allowlist.txt`](../../scripts/ci/twin-drift-allowlist.txt)
   with a reason.
2. Any build file names a source path (`.c .cpp .cc .cxx .cu .hip .m .mm .metal
   .pyx`) that does not exist in the tree.

### How references are resolved

| Build-file form | Resolution |
| --- | --- |
| `'../src/x.c'`, `'x.c'` | relative to the build file's directory |
| `src_dir + 'x.c'` | through the `src_dir = './.../'` assignment in the same file |
| `_m + '_parity.c'` (prefix is not a literal directory) | suffix search over `git ls-files`; reported as `NOTE` |
| `os.path.join("..", "core", "x.c")` | joined; identifiers resolve through assignments |
| `output: 'gen.c'`, `'@PLAINNAME@.c'` | skipped; generated files |
| `/abs/path.c` | skipped; toolchain-provided |
| `# ...` comments | ignored (quote-aware) |

### Clearing a failure

- **Stale source reference:** fix the path in the build file. There is no
  allowlist for this class. If the parser genuinely cannot model a construct,
  append `# twin-drift-ignore: <reason>` to that line; the reason is mandatory
  and the line is reported as `NOTE`.
- **Dead twin side:** wire the side into a build file, delete it, or add a row
  `path  reason` to the allowlist. Rows without a reason, rows whose file is
  gone, whose side is compiled again, or whose pair no longer exists fail the
  gate, so the allowlist cannot rot.

Sides compiled only by test or fuzz build files are printed as `INFO`
(non-failing): that is the drift-risk shape to keep an eye on when touching one
of them.

The local run is identical to CI, takes about two seconds, needs no build, and
is also wired as a `pre-push` hook:

```bash
bash scripts/ci/twin-drift-check.sh
bash scripts/ci/tests/test-twin-drift-check.sh   # 24 fixture cases
```

## Lint, standards and bookkeeping gates

These sections keep their anchors for existing links; the content lives on the
pages named here.

### Whole-tree lint ratchet (ADR-1142)

CI bounds clang-tidy findings for every file with a per-lane baseline
(`scripts/ci/tidy-baseline-<lane>.json`): a file may never exceed its baseline,
and a cleaner file must tighten it in the same PR. The required context is
`Tidy Ratchet` (the `cpu` lane); the `cuda`, `sycl`, `hip` and `arm64` lanes are
measured in the dev container and are not PR-required. Rules, commands and
exit codes are in [Tidy ratchet](tidy-ratchet.md); measuring is in
[measuring the lanes](tidy-lanes.md).

### Carve-outs still open after ADR-1142

The open scope restrictions of the lint configuration (which paths
`Tidy Changed` excludes, and what blocks each) are in the
[carve-out table of the tidy ratchet page](tidy-ratchet.md#carve-outs-still-open-after-adr-1142).

### Local lint build profile and receipts

`make lint` against a configured build, its receipt directory and the cppcheck
model files are in [Local lint](local-lint.md).

### Praetor documentation gate

`make docs-lint` and `make docs-figures` are praetor's Documentation Governance
gate, and praetor owns several files byte for byte. See
[Praetor gate](praetor-gate.md#praetor-documentation-gate).

### Moving the praetor pin

`PRAETOR_REF` in `standards-gate.yml` names the engine CI installs; the move
procedure is in [Praetor gate](praetor-gate.md#moving-the-praetor-pin).

### Resolving a `docs/state.md` rebase conflict

Run `python3 scripts/dev/resolve-state-md-conflict.py docs/state.md` mid-rebase;
see [state.md gates](state-md-gates.md#resolving-a-statemd-rebase-conflict).

### Bug-status hygiene gate (ADR-0165 / ADR-0334)

A `fix:` PR, a `bug` title or a close keyword must touch `docs/state.md` (rule
15 of the [agent hard rules](agent-hard-rules.md)), or say
`no state delta: REASON` in the body. See
[state.md gates](state-md-gates.md#bug-status-hygiene-gate-adr-0165-adr-0334).

## Flaky legs (2026-09-04 audit)

An audit of CI runs on `master` across all workflows found two leg reliability
issues, addressed under epic #1236. Counts were taken with
`gh run list --branch master --limit 40` on 2026-09-05:

| Leg | Symptom | Frequency | Cause | Status |
| --- | --- | --- | --- | --- |
| `tests-and-quality-gates.yml` / MCP Smoke (Embedded C + Python Server) | `test_mcp_smoke TIMEOUT 60.04s`, killed after test 15 (`test_uds_roundtrip`) | 1 of 40 master runs (27 passed, 11 cancelled, 1 failed) | `stop_uds()` closed the AF_UNIX listener without `shutdown()`; on Linux that never wakes a worker already blocked in `accept(2)`, so `pthread_join()` hung. A race: only when the worker re-entered `accept()` before `close()`. | Real bug, fixed in `ci/flaky-legs-1236` (`T-CI-MCP-SMOKE-TIMEOUT-2026-09-04`) |
| `build.yml` / `libvmaf-build-matrix.yml` macOS `brew install ... llvm` | Homebrew bottle download failure | 0 of 40 `build.yml` runs (25 passed, 14 cancelled on master; 0 download failures) | GitHub-hosted runner network or Homebrew CDN | Infrastructure; hardened with a 3-attempt retry plus `brew fetch --retry` (`T-CI-MACOS-BREW-LLVM-FLAKE-2026-09-04`) |
| `release-please.yml` | Fails on every master push | 3 of 3 master runs in the window | Missing GitHub App credentials | Infrastructure; tracked in #1289 |
| Every other workflow on master | None | 0 failures in the last 40 master runs (one `CI` run was cancelled by a superseding push, not failed) | n/a | Nothing to fix |

### MCP smoke timeout (run 33916590280, commit `0a8727ca7`)

The `MCP Smoke (Embedded C + Python Server)` job hit the 60-second Meson
timeout during `test_uds_roundtrip`.

- **Root cause:** in `core/src/mcp/mcp.c`, `stop_uds()` called
  `close(server->uds_listen_fd)` without `shutdown(server->uds_listen_fd,
  SHUT_RDWR)` first. On Linux, closing a listening `AF_UNIX` stream socket does
  not unblock a concurrent `accept(2)` in the worker thread
  (`vmaf_mcp_uds_thread_main`), leaving the thread asleep in the socket wait
  queue while `stop_uds()` waited indefinitely on `pthread_join()`.
- **Measured passing distribution:** across 20 iterations `test_mcp_smoke`
  takes about 0.03 s (range 0.01 s to 0.05 s). The 60 s timeout was a true
  deadlock, not a slow test.
- **Fix:** `shutdown(server->uds_listen_fd, SHUT_RDWR)` before `close()` in
  `stop_uds()`, mirroring the existing SSE listener shutdown contract, plus
  defensive early-stop guards in `vmaf_mcp_uds_thread_main`.

### macOS Homebrew `llvm` download reliability (`build.yml`)

- **Symptom:** intermittent network dropouts or CDN blips when downloading large
  Homebrew bottles (such as `llvm`, about 500 MB) on GitHub-hosted macOS
  runners. The audit of 40 recent `build.yml` runs found 0 failures, so the
  baseline rate is low, but network flakiness is an established infrastructure
  failure mode.
- **Mitigation:** the Homebrew installation in `build.yml` and
  `libvmaf-build-matrix.yml` runs in a 3-attempt retry loop with backoff (10 s,
  20 s) and fallback `brew fetch --retry`. `HOMEBREW_NO_AUTO_UPDATE=1` and
  `HOMEBREW_NO_INSTALL_CLEANUP=1` are exported to avoid costly auto-updates and
  cleanup consuming runner time.

### Other master workflow legs

The last 40 and 100 master runs across all workflows showed no other flaky test
or build failure; the only other master failure was the `release-please` token
permissions, tracked in #1289.
