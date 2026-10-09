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
| [`ci-tier.yml`](../../.github/workflows/ci-tier.yml) | Reusable workflow every pull-request workflow calls first: decides the tier ([ADR-2169](../adr/2169-ci-fewer-runs.md)); no trigger of its own. |
| [`ci-escalate.yml`](../../.github/workflows/ci-escalate.yml) | Re-runs the suite at the full tier when `ci: full` or `autorelease: cut` is applied. |
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
| [`upstream-consumers.yml`](../../.github/workflows/upstream-consumers.yml) | Unpatched upstream FFmpeg and GStreamer against the installed compat `libvmaf.so.3`, scores compared with the CLI and the base commit (full tier; see [upstream consumers](upstream-consumers.md)). |
| [`sycl-parity.yml`](../../.github/workflows/sycl-parity.yml) | SYCL parity on the self-hosted Intel Arc A380 runner (ADR-1177; see the [runbook](ci-self-hosted-sycl.md)). |
| [`docs.yml`](../../.github/workflows/docs.yml) | Docs build. |
| [`doxygen-public-api.yml`](../../.github/workflows/doxygen-public-api.yml) | Doxygen build of the public C API; required since ADR-1297. |
| [`docker-image.yml`](../../.github/workflows/docker-image.yml) | Docker image build. |
| [`release-dry-run.yml`](../../.github/workflows/release-dry-run.yml) | Builds the release images and the `vmaf-mcp` distribution and SBOM without publishing (ADR-1595); required context `Release Dry Run` on pull requests ([ADR-1687](../adr/1687-required-release-dry-run-legs.md)). |
| [`windows-tester-bundle.yml`](../../.github/workflows/windows-tester-bundle.yml) | The Windows tester zips: the x64 zip on a pull request, all four on a push, published on dispatch; required context `Windows Tester Zip` (ADR-1687). |
| [`dev-container-build.yml`](../../.github/workflows/dev-container-build.yml) | PR-time build gate for `dev/Containerfile` (ADR-0819). |
| [`helm-chart.yml`](../../.github/workflows/helm-chart.yml) | `helm lint` of the chart. |
| [`rust-ci.yml`](../../.github/workflows/rust-ci.yml) | Rust crates: `cargo fmt --all` and `clippy --workspace`, `cargo test --workspace`, the golden smoke example and `cargo-deny`; the planner may skip the work, the gates `vmafx-sys CI` and `cargo-deny` are required. |
| [`sanitizers.yml`](../../.github/workflows/sanitizers.yml) | Combined ASan and UBSan on full-tier pull requests and master pushes, TSan on master pushes, nightly fuzzing. |
| [`praetor-docs.yml`](../../.github/workflows/praetor-docs.yml) | Praetor's Documentation Governance gate for the `docs:seo-portal` facet; praetor-managed, not required. See [Praetor gate](praetor-gate.md). |
| [`praetor-api.yml`](../../.github/workflows/praetor-api.yml) | Praetor's `Go API Compatibility` gate (`go-apidiff` over every Go module; no path filter). Praetor-managed; required through the aggregator (ADR-1506), its marker sits in `standards-gate.yml`. |
| [`reuse.yml`](../../.github/workflows/reuse.yml) | Praetor's `REUSE lint` gate (`reuse lint` over the whole tree; no path filter), the one CI run of `reuse lint`. Written by praetor, its actions pinned to commits here; required through the aggregator (ADR-2784), its marker sits in `standards-gate.yml`. See [Praetor gate](praetor-gate.md#the-current-pin). |
| [`scorecard-policy.yml`](../../.github/workflows/scorecard-policy.yml) | OpenSSF Scorecard PR policy (ADR-1247). |
| [`pr-type-label.yml`](../../.github/workflows/pr-type-label.yml) | Derives a `type:*` label from the Conventional-Commit prefix of the PR. |

### Scheduled, release and watcher workflows

| File | Purpose |
| --- | --- |
| [`nightly.yml`](../../.github/workflows/nightly.yml) | Nightly jobs, including the whole-tree clang-tidy ratchet of the `cpu` lane and the sweep of every other lane (ADR-2796). |
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
| [`research-radar.yml`](../../.github/workflows/research-radar.yml) | Weekly digest of changes in the public video-quality sources of the [research radar](../research/radar/README.md) ([ADR-2171](../adr/2171-research-radar.md)). |

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
- the first job of the workflow, `tier`, carries an `if:` clause of the form
  `github.event_name != 'pull_request' || github.event.pull_request.draft == false`
  and every other job needs it ([ADR-2169](../adr/2169-ci-fewer-runs.md); before
  that each job carried the clause itself). The workflows that cannot carry it
  are the two praetor-locked ones, listed under
  [Which jobs run when](#which-jobs-run-when-adr-2169).

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

## Which jobs run when (ADR-2169)

A pull request does not owe the whole suite. The tier of an event decides which
required contexts run; one file, [`.github/ci-tier.json`](../../.github/ci-tier.json),
defines the tiers, and the workflows and the aggregator both read it.

| Event | Tier | What runs |
| --- | --- | --- |
| Pull request from a fork | full | everything |
| Pull request from this repository (Renovate included) | light | lint, format, the fast suite and the governance gates |
| Own pull request with the label `ci: full` | full | everything |
| Release pull request (`release-please--` branch, bot author or the maintainer account with a release-only diff) | release-light | `Release Script Contract` |
| Release pull request with the label `autorelease: cut` | full | everything |
| Draft pull request | none | nothing, but the aggregator, which fails it |
| Push to `master`, dispatch, schedule | full | everything |
| Push to another branch, push of a tag | none | nothing |

The light tier is every required context except the `full_only` list of
`ci-tier.json`: the platform legs of the build matrix (the Ubuntu `gcc+DNN` and
`clang+DNN` legs stay), the all-backend `Build` lanes, Windows and macOS legs,
GPU builds, coverage, sanitizers, the dev container, docker image, FFmpeg
integration, tester and release dry-run lanes, and the self-hosted hardware lanes.
Those run on the master push, so a break in them is found there, after the merge
train landed the change. To see one on a pull request, add the label `ci: full`.

What a contributor sees:

- Every workflow shows a job `CI tier (<workflow>) / Decide the CI tier` first;
  the other jobs of the workflow appear when it completes (GitHub creates a job
  that needs another only then), and the aggregator waits for the tier decisions
  before it judges which contexts are missing.

- A job the tier does not run shows as skipped. The aggregator accepts a skipped
  or absent context the tier does not owe, and still fails one that ran and
  failed. The matrix legs of `libvmaf-build-matrix.yml` are the one place a job
  starts and stops: a matrix cannot be filtered per leg, so a full-tier leg on a
  light-tier pull request ends after a shallow checkout with a notice.
- Adding the label `ci: full` (or `autorelease: cut` to the release pull request)
  starts `ci-escalate.yml`, which re-runs the latest run of each workflow on the
  head commit. Every re-run decides its tier again from the live labels, so the
  skipped jobs now run and the aggregator waits for them. Removing the label does
  not cancel anything; the next push is light again.
- The release pull request is refreshed on every merge to master. Without the
  cut label it runs `Release Script Contract` only, which is the check that proves
  the cut ran. The maintainer applies `autorelease: cut` when the release is
  to be cut, and the full suite runs once on the exact head.
- A pull request from a fork always runs the full tier: the head repository is
  compared with `github.repository` in `scripts/ci/ci_tier.py`.
- A branch named `release-please--...` is not enough to be the release pull
  request: `scripts/ci/release-pr-exempt.sh` checks the author and, for the
  maintainer account, that the diff touches only release files (ADR-1151,
  ADR-1388). Anything else is an ordinary own pull request.

How a workflow takes part: its first job is `tier`, a call of
[`ci-tier.yml`](../../.github/workflows/ci-tier.yml). Light-tier jobs gate on
`needs.tier.outputs.light == 'true'`, full-tier jobs on
`needs.tier.outputs.full == 'true'`; a planner workflow gates its `impact` job and
its gate job, and a gate job is `always() && needs.tier.outputs.<tier> == 'true'`.
The `tier` job also carries the draft gate of the previous section. The
exceptions (jobs without the tier, each with a reason and an expiry) are the
`untiered_jobs` of `ci-tier.json`: the aggregator, `Release Script Contract`, the
label and escalation workflows, the opt-in e2e gate, and the praetor-managed
`praetor-api.yml` and `praetor-docs.yml`. The last two are locked byte for
byte by `praetorctl audit`. Since the pin `7458a220e1c9` (praetor#815) they push
only on `master` and listen for `opened`, `synchronize`, `reopened` and
`ready_for_review`, so they no longer start on a push to another branch. On a
draft their first step fails closed and every later step is skipped: the job
starts and reports a red check but does no work. A job-level gate cannot be
added to a byte-locked file, so they stay in `untiered_jobs`, and
`test_praetor_managed_jobs_stop_on_a_draft_before_any_work` holds the step
shape. `push_branch_exceptions` is empty.

Adding a required context: add it to the aggregator `required` list and the
`# required-aggregator` marker as before, and to `full_only` or `always` in
`ci-tier.json` if it is not a light-tier context. The routing contract fails
when the workflows and the file disagree.

### The routing contract

`scripts/ci/tests/test_ci_routing_contract.py` is the proof. It builds synthetic
events (own, fork, Renovate, release with and without the cut label, a person on
a release branch name, draft, master push, a push to a feature branch, a tag, an
unrelated label, an escalation label), works out with
`scripts/ci/ci_router.py` which jobs of the real workflow files run
(`scripts/ci/ci_expressions.py` evaluates the `if:` expressions as the Actions
documentation defines them, and the tier decision is the real `ci_tier.py`),
and compares that with `ci-tier.json`. Six more cases plant a defect in a copy
of the workflows and require the contract to fail. To see what the contract
checks against another tree, point it at a workflow directory:

```bash
CI_ROUTING_WORKFLOWS_DIR=<tree>/.github/workflows \
  python3 -m unittest scripts.ci.tests.test_ci_routing_contract
```

The aggregator side is `test_ci_aggregator_tier.py`, the decision is
`test_ci_tier.py`, and the escalation is `test_ci_escalate.py`, all in
`scripts/ci/tests/`.

### Renovate

[`renovate.json`](../../renovate.json) runs in a weekly window (Monday before 6am,
Europe/Vienna). A first catch-all rule puts every minor, patch, digest and pin
update that no other rule groups into one pull request; the existing ecosystem
rules keep their own groups, automerge and review settings; major updates stay
individual and manual; `rebaseWhen` is `conflicted`, so a Renovate branch is not
rebuilt each time master moves. Security updates (`vulnerabilityAlerts`) are
opened at any time and are not grouped. Renovate pull requests come from this
repository and run the light tier.

## Gates judge their own matrix leg

A matrix job's `needs.<job>.result` is the aggregate of every leg, so a gate
that reads it fails for a leg it does not name. The gates that share a matrix
(`Linux Intel LLVM`, `macOS Clang+Metal` and `Windows MSVC+CUDA (full)` in
`build.yml`; `FFmpeg Ubuntu gcc` and `FFmpeg macOS clang` in
`ffmpeg-integration.yml`) therefore run
[`scripts/ci/gate_leg_result.py`](../../scripts/ci/gate_leg_result.py) with the
name of their own `<check name> work` job. It reads the run's jobs
(`gh api repos/<repo>/actions/runs/<id>/jobs --paginate`, which needs
`actions: read`) and passes when the planner succeeded and either the leg was
selected and concluded `success`, or it was not selected and is absent or
`skipped`. A missing, unfinished or ambiguous own job fails. A new gate on a
shared matrix does the same; `scripts/ci/tests/test_gate_leg_result.py` fails
on two gates that read one matrix job's aggregate.

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
the selectors declared in `.github/ci-impact.json`. There are 25 selectors:

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
| `tidy_cuda` | `core/src/cuda/`, `core/src/feature/cuda/`, the CUDA and GPU tests, `core/src/dnn/` and the ratchet, lane baseline and lane scripts; own paths only ([ADR-2796](../adr/2796-hosted-tidy-all-lanes.md)) | `Tidy Lane (cuda)` |
| `tidy_hip` | `core/src/hip/`, `core/src/feature/hip/`, the HIP and GPU tests, the same lane infrastructure; own paths only | `Tidy Lane (hip)` |
| `tidy_sycl` | `core/src/sycl/`, `core/src/feature/sycl/`, the SYCL and GPU tests, `core/tools/vmaf_vpl*`, the same lane infrastructure; own paths only | `Tidy Lane (sycl)` |
| `tidy_arm64` | `core/src/feature/arm64/`, `core/src/arm/`, the NEON and SVE tests, the aarch64 cross files, the same lane infrastructure; own paths only | `Tidy Lane (arm64)` |
| `tidy_clang` | `core/test/fuzz/`, `core/src/read_json_model.c`, the same lane infrastructure; own paths only | `Tidy Lane (clang)` |
| `tidy_metal` | `core/src/metal/`, `core/src/feature/metal/`, the Metal tests, `tidy-metal.yml`, the ratchet and the Metal baseline; own paths only | `Tidy Metal` |
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
read), it stays true. Only the tester selectors and the six `tidy_<lane>`
selectors carry it ([ADR-2796](../adr/2796-hosted-tidy-all-lanes.md)), it
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

These are full-tier contexts, with one declared exception
([ADR-2198](../adr/2198-windows-sycl-leg-and-cut-check.md)): `Windows Tester
Zip` is an **own-input lane** (`own_input_lanes` of `.github/ci-tier.json`), so
a pull request from this repository also runs it, but its planner builds only
what the diff touches. Selector `windows_tester_zip` builds the x64 zip;
selector `windows_tester_zip_sycl` (everything the x64 zip reads, plus
`sycl-rows.json`, `prepare_build.py`, `build-config.env` and the SYCL scratch
ratchet list) adds the x64 SYCL zip. The arm64 and CUDA zips, `Tester Image`'s
arm64 leg and the macOS bundle still build on the master push or a dispatch
only, which is why a cut checks them (`scripts/release/check-candidate-legs.py`,
see [the release
guide](release.md#before-the-cut-every-tester-leg-green-on-the-commit)). For the
other contexts a pull request from this repository runs them only with the label
`ci: full` (see [Which jobs run when](#which-jobs-run-when-adr-2169)), and the
master push is where they run for every change. An unselected tester run passes
in about a minute. Which inputs select them and how to reproduce a failure:
[verifying the release and tester workflows](release-workflow-verification.md).

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

The store tests start PostgreSQL in a container through testcontainers
(`cmd/vmafx-controller/store/storetest`, images pinned by digest). Before
`go test`, the job runs `scripts/ci/docker-hub-mirror.sh`. The script adds
`https://mirror.gcr.io` to the runner Docker daemon's `registry-mirrors` and
restarts the daemon. It fails if `docker info` does not show the mirror. The
change applies to every `docker.io` pull of the job, and Docker Hub's anonymous
pull limit no longer fails the tests. The pinned digests stay valid, because
the mirror serves the same content addresses. `DOCKER_HUB_MIRROR` overrides the
mirror. `scripts/ci/tests/test_docker_hub_mirror.py` checks the `daemon.json`
merge offline (`--print`). Golusoris uses the same pattern.

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
`Tidy Ratchet` (the `cpu` lane). Every other lane is a required, path-routed
check as well ([ADR-2796](../adr/2796-hosted-tidy-all-lanes.md)):
`Tidy Lane (cuda)`, `Tidy Lane (hip)`, `Tidy Lane (sycl)`,
`Tidy Lane (arm64)` and `Tidy Lane (clang)` (legs of one job in
`lint-and-format.yml`, measured in the pinned dev container) and `Tidy Metal`
(`tidy-metal.yml`, a macOS runner). Each starts on every pull request and
measures its lane only when the planner selects `tidy_<lane>`; a lane it did
not measure says so and is checked by the nightly sweep, which runs all lanes
on master and opens or updates one issue on drift. Rules, commands and
exit codes are in
[Tidy ratchet](tidy-ratchet.md#hosted-lanes-adr-2796); measuring is in
[measuring the lanes](tidy-lanes.md).

### Carve-outs still open after ADR-1142

The open scope restrictions of the lint configuration (which paths
`Tidy Changed` excludes, and what blocks each) are in the
[carve-out table of the tidy ratchet page](tidy-ratchet.md#carve-outs-still-open-after-adr-1142).

### Source ADR citations (ADR-1311, ADR-2200)

`scripts/ci/check-source-adr-citations.py` binds each plain `ADR-NNNN` in
source and build files to the one file `docs/adr/NNNN-*.md`. It derives the
binding from the tree on every run, so citing an ADR needs no registry edit;
`scripts/ci/source-adr-citations.json` holds only the hand-written `retired`
and `fixtures` records. The gate fails on a number that has no ADR file and no
retirement record (file the ADR, or audit and record the retirement), on a
retired number that has a file again, and on a fixture number cited outside its
recorded paths.

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

## Warnings are errors (ADR-2170, ADR-2828)

HISS-10 asks for zero warnings, so every CI lane that compiles C, C++, Rust or
Go turns warnings into errors: the pull request that adds a warning fails the
lane that prints it. Each lane spells its switch on the build command itself,
where praetor's build-warnings gate (`praetorctl audit`,
[Praetor gate](praetor-gate.md#the-current-pin)) reads it:

| Toolchain | Switch on the lane | Linker half |
| --- | --- | --- |
| Meson (gcc, clang, icx/icpx, MinGW, cl.exe, icx-cl, nvcc, hipcc) | `-Dwerror=true` on `meson setup` (`-Werror`, `/WX`; `core/src/meson.build` adds `--Werror all-warnings` for nvcc and `-Werror` for hipcc) | `$(scripts/ci/werror-args.sh true)` beside it |
| CMake (the Level Zero loader the SYCL legs build) | `-DCMAKE_COMPILE_WARNING_AS_ERROR=ON` on the configure | none |
| A direct compile (the static pkg-config link smoke) | `-Wall -Wextra -Werror`, compiler named in the step's `env` | none |
| Cargo (`rust-ci.yml`) | `RUSTFLAGS: -D warnings` on the job, beside `cargo clippy -- -D warnings` | none |
| Go with cgo (`go-ci.yml`) | `CGO_CFLAGS: -O2 -g -Wall -Werror` on the job, beside `go vet` | none |

[`scripts/ci/werror-args.sh`](../../scripts/ci/werror-args.sh) stays the one
place that knows each linker's fatal-warnings switch. With `true` it prints
`-Dwerror=true` again and `-Dc_link_args` / `-Dcpp_link_args` with
`-Wl,--fatal-warnings` (GNU ld, lld, MinGW) or `-Wl,-fatal_warnings` (Apple
ld64). On macOS the C++ link arguments also carry
`-no_warn_duplicate_libraries`: Meson 1.12 detects the C++ standard library of
the Objective-C++ (Metal) build by linking a probe with `-lc++`, which the
clang++ driver adds again, and ld64's duplicate-library warning would fail
`meson setup` ("Could not detect either libc++ or libstdc++");
`core/src/metal/meson.build` gives the project's own links the same switch.
With `msvc` it prints `-Dwerror=true` alone: Meson adds `-WX` to every
`link.exe` link itself. The MSVC legs run their steps under `cmd`, so a
`shell: bash` step named `Warnings-as-errors arguments` (`id: werror`) calls
the script and the configure step appends `${{ steps.werror.outputs.args }}`;
their literal sits on the first line of the `meson setup` command, because the
gate does not read a `cmd` line continuation. `lib.exe` has no fatal-warnings
switch in Meson. Release and container image builds do not use any of this: a
compiler newer than the one a leg pins must not stop a release over a new
diagnostic. The tidy and cppcheck lanes compile with the switch, and
[`scripts/ci/write-compile-commands.py`](../../scripts/ci/write-compile-commands.py)
drops `-Werror` from the database those analysers read, so clang-tidy sees the
flags it saw before.

A fix for a warning changes no computed value and suppresses nothing: no
`-Wno-*`, no `#pragma ... ignored`, no flag removed to hide a class. A
diagnostic that is a defect of the tool needs a declared exception (one file,
one rule, a reason, an expiry). The `HISS-10` exception list is empty
([ADR-2828](../adr/2828-ci-werror-every-lane.md)).

### Lanes that are gated

Every lane `praetorctl audit` reads: 50 in 16 workflows (Meson 35, Cargo 7,
CMake 5, Go 2, one direct compile).

| Workflow | Lanes | Toolchain family |
| --- | --- | --- |
| `libvmaf-build-matrix.yml` | every Linux and macOS row, Windows UCRT64, Windows MSVC+CUDA, Windows MSVC+SYCL, Windows ARM64 MSVC, the Level Zero builds and the static link smoke | gcc 14, clang 22, Apple clang, icx / icpx, MinGW gcc, MSVC `cl.exe`, icx-cl, nvcc, hipcc |
| `build.yml` | Linux Intel LLVM, macOS Clang+Metal, Windows MSVC+CUDA (full), the Level Zero build | icx / icpx, Apple clang, MSVC, nvcc |
| `sanitizers.yml`, `tests-and-quality-gates.yml` (sanitizer matrix) | ASan+UBSan, TSan, address, undefined, thread, nightly libFuzzer | clang 22, lld |
| `tests-and-quality-gates.yml` | Netflix CPU golden, DNN, MCP smoke, coverage (gcov), coverage GPU | gcc, icx / icpx with nvcc |
| `lint-and-format.yml`, `tidy-metal.yml`, `security-scans.yml` | the builds behind Tidy Changed, Tidy SYCL, Cppcheck, Tidy Metal and CodeQL C/C++ | gcc 15, icx, Apple clang, gcc |
| `nightly.yml`, `fuzz.yml`, `mini-retrain.yml`, `sycl-parity.yml` | TSan, Netflix benchmark, libFuzzer, retrain CLI, Arc A380 parity | gcc, clang 22, icx |
| `upstream-consumers.yml` | libvmaf at this commit and at the base commit | gcc |
| `go-ci.yml`, `rust-ci.yml` | the libvmaf build, the cgo package, every cargo step and the Rust-extractor build | gcc, rustc, Go |
| `ffmpeg-integration.yml` | the libvmaf builds (Ubuntu, macOS, SYCL, Windows MSVC) and the Level Zero build | gcc, clang, icpx, MSVC |
| `windows-tester-bundle.yml` | the Level Zero build | MSVC |

### Builds outside the gate

| Build | Warnings | Why |
| --- | --- | --- |
| Dev Container Build | 204 on master `70d6dd0a5` | third-party sources built in the image (vpl-gpu-rt `-Wstringop-overflow`, FFmpeg) and gcc's LTO "serial compilation" note; the image is a container build, not a lane the gate reads (`T-CI-WARNINGS-NON-MSVC-LEGS-2026-10-07`) |
| Release and tester images, release binaries | none measured | kept off warnings-as-errors by ADR-2170 (above) |

### What has been proven

Each toolchain family failed a planted warning once and passed again once it
was removed (2026-10-07, the same build directories configured with the gate's
arguments): gcc, clang, the MinGW cross compiler, clang for aarch64 and icx /
icpx for `-Wunused-variable` and `-Wunused-function` in a C and a C++
translation unit; nvcc (`--Werror all-warnings`, diagnostic `#177-D`) and
hipcc (`-Werror`, `-Wreturn-type`) in a device kernel; GNU ld
(`--fatal-warnings`, "requires executable stack"). Not proven on this host,
only on the CI run of the pull request that gates them: Apple clang and ld64
(`-fatal_warnings`), and lld (`--fatal-warnings`, the sanitizer legs: the
compiler half is proven by clang, no default-on lld warning was found to
plant).

The MSVC legs were proven on CI, on throwaway branches of the gate commit
dispatched with `workflow_dispatch` (2026-10-07). `float planted = 0.1;` in
`core/test/test_picture.c` failed `Windows MSVC+CUDA`, `Windows ARM64 MSVC`
and `Windows MSVC+CUDA (full)` with C4305 as error C2220 (runs 37635562015 and
37635566701); `#pragma comment(linker, "/VMAFXPLANTEDDIRECTIVE")` failed the
`test_picture.exe` link of `Windows MSVC+CUDA` and `Windows ARM64 MSVC` with
LNK4229 as error LNK1218 (run 37635571313). The gate commit itself passed
all three legs with `werror : true` in the Meson summary, `-WX` on the link
lines and no warning in the logs (run 37635551653: the `Windows MSVC+CUDA`
build, and the `Windows ARM64 MSVC` build with its 355 fast tests; run
37635557102: the `Windows MSVC+CUDA (full)` build and its CPU tests).

The macOS Metal leg links every target with the Objective-C++ compiler, and
Meson 1.12 then names `-lc++` twice (224 ld64 warnings per run). The clang++
driver adds the library itself, so duplicates are harmless;
`core/src/metal/meson.build` passes `-Wl,-no_warn_duplicate_libraries` to
those links, with the reason beside it, and the leg is gated. The deliberate
register spill of the SYCL self-test kernel is compiled through
`core/src/sycl/run_captured.py`, which prints the device compiler's output
only when the compile fails.

The lanes ADR-2828 gated (2026-10-09), proven on this host: gcc 15 in the
dev image built the golden configuration (release, LTO, float, AVX-512) and
the coverage configuration (gcov, DNN) clean with the gate's arguments, and
failed both on a planted unused variable in `core/src/libvmaf.c`; gcc 16 built
the Rust-extractor configuration (`RUSTFLAGS: -D warnings`) and the Level Zero
loader 1.34.0 (`-DCMAKE_COMPILE_WARNING_AS_ERROR=ON`, which CMake turns into
`-Werror`) clean; the cgo package failed a planted `-Wunused-variable` under
`CGO_CFLAGS=-O2 -g -Wall -Werror` and both cargo workspaces a planted unused
variable under `RUSTFLAGS=-D warnings`; the static link smoke compiles clean
with `-Wall -Wextra -Werror` under gcc 13, 15 and 16.

On CI, a throwaway branch carrying
`int vmafx_planted_warning(void) { int unused; float narrowed = 0.1; ... }`
in `core/src/libvmaf.c` (2026-10-09) failed on the plant in every leg that
compiled it: every Linux and macOS row of the build matrix, Windows UCRT64,
Windows MSVC+CUDA and Windows ARM64 MSVC (C4101 and C4305 as C2220) and
Windows MSVC+SYCL (icx-cl, `-Wunused-variable` as an error) in run
37856727687; Linux Intel LLVM and Windows MSVC+CUDA (full) in run
37856730893; Tidy Metal (Apple clang) in run 37856734080; the DNN and MCP
smoke builds in run 37856740709. The same run stopped the macOS Clang+Metal
leg at `meson setup`, which the ld64 switch above fixes; the static link smoke
under `-Wall -Wextra -Werror` reported three `-Wmaybe-uninitialized` sites of
the LTO archive (`float_vif.c`, `speed.c`, `iqa/convolve.c` through `ssim.c`),
fixed at the source and reproduced and cleared with gcc 15 here.

The same plant on the rebased branch (2026-10-09, commit `43f1fe0b4`) failed
the remaining lanes once their own blockers were gone: macOS Clang+Metal,
Linux Intel LLVM and Windows MSVC+CUDA (full) in run 37914112985; CodeQL C/C++
in run 37914119958; the Netflix golden, coverage, DNN, MCP smoke and the three
sanitizer-matrix builds in run 37914123154; the FFmpeg Windows MSVC, Ubuntu,
macOS and SYCL builds in run 37914126479; and every leg of the build matrix,
macOS Metal included, in run 37914116846. The pull request's own run of the
FFmpeg Windows MSVC job (job 113771388651) configured with `werror : true`,
printed none of the pkg-config name warnings, and passed
`check_msvc_library_names.py --meson-log`. The Arc A380 runner was offline, so
SYCL parity and coverage GPU are proven by the SYCL and CUDA legs above until
their next run.

### Adding a leg

1. Build the leg's configuration on a clean tree and count the warnings by
   unique (file, line, flag), not by lines of log (a header diagnostic repeats
   once per translation unit that includes it).
2. Fix every site; run the Netflix golden gate and the fast suite.
3. Spell the switch on its build command as the first table above says
   (`-Dwerror=true $(scripts/ci/werror-args.sh true)` on a `meson setup`;
   `werror: true` on a build-matrix row; `werror: msvc`, the `id: werror` bash
   step and `-Dwerror=true` on the first `meson setup` line for an MSVC leg),
   add it to the gated table, and run `praetorctl audit` and
   `python3 -m unittest scripts/ci/tests/test_werror_args.py`. The audit fails
   a lane it reads without the switch; the test fails a build-matrix row or an
   MSVC leg that is not gated.
4. Prove the gate once: plant `int planted(void) { int unused; return 0; }`
   (for MSVC, `float planted = 0.1;`, C4305) in a throwaway branch and see the
   leg fail; record the run in the pull request.

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
