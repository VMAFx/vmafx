# Agent notes — `.github/` (workflows + templates)

Parent: [../AGENTS.md](../AGENTS.md).

This directory holds GitHub-facing config: Actions workflows, issue /
PR templates, CODEOWNERS file. Everything here is fork-local.
Netflix/vmaf upstream has its own `.github/` that rarely overlaps
path-wise. Conflicts on merge tend to be rare but high-impact
when they happen (silently-broken workflow is less visible than
broken `.c` file).

## Invariants a reviewer or sync must preserve

### Single SemVer release fan-out (ADR-1127)

`release-please.yml` owns one root package, creates draft GitHub release.
Publishing that draft is authenticated operation that creates `vX.Y.Z`
tag, starts `supply-chain.yml` plus both Docker publication workflows. Never
split `release-please` back into unqualified component tags or make release
non-draft without first providing and validating explicit downstream
workflow trigger. Workflow pauses both `release-please` phases while one
ordinary-SemVer draft exists; later master pushes must not move or duplicate
release waiting at human publication gate. Initial 3.2.1 cut is selected
by one-time `release-as` config field; release-PR rollover must remove that
field and `bootstrap-sha` before release PR merges so neither override can
affect 3.2.2.

Every job publishing to GHCR, uploading GitHub Release assets, or minting
release-artifact Sigstore identity is bound to protected
`release-publish` environment. PyPI stays bound to `pypi-publish` because that
exact environment name is part of its Trusted Publisher identity. Both
environments accept ordinary release tags only, require configured
release reviewer; read-only validation stays outside them. SLSA reusable
workflow caller cannot declare environment, so it has `contents: read` and
`upload-assets: false`; environment-gated attachment job is sole
release-asset writer. Never restore direct SLSA release upload.

`supply-chain.yml` runs `scripts/release/verify-release-version.sh` before any
write or OIDC job. It keeps native and `vmaf-mcp` hashes in distinct SLSA jobs
with distinct provenance asset names. Native SBOMs contain and hash every
staged artifact; Python SBOMs inventory installed `vmaf-mcp` dependency
graph plus wheel and sdist. Workflow fails if either inventory becomes
empty or mislabeled. Anchore's implicit artifact/release uploads stay disabled:
explicit SBOM artifact feeds keyless signing before final strict
attachment job. Never bypass that DAG or restore permissive unmatched-file
uploads; green workflow must mean every promised asset exists.

Native payload is Linux ELF, materializes complete Meson
`libvmaf.so` / SONAME / real-name chain as regular files. Before any native
write or OIDC job, artifact round-trip verifier must prove
downloaded `vmaf` resolves its declared SONAME from that directory, reports
release version under `env -i`. Hashing, SBOM generation, signing, SLSA
provenance, and strict attachment cover every materialized chain name.

Manual supply-chain recovery must use published tag as both workflow ref
and input (`gh workflow run supply-chain.yml --ref "$tag" -f tag="$tag"`).
Validation job rejects branch-ref dispatches, missing/draft/prerelease releases,
or event SHA different from checked-out tag. This binding is required so
SLSA provenance describes source that produced artifacts. Container
signature verification requires exact `@refs/tags/${PUBLISH_TAG}` workflow
identity; wildcard ref would accept signatures minted by branch workflows.

### Rule-enforcement split (ADR-0124)

[`rule-enforcement.yml`](workflows/rule-enforcement.yml) has three
jobs. Only **one** is allowed to be required-status-check-blocking:

- `deep-dive-checklist` — **blocking**. Predicate is mechanically
  decidable (ticked checkboxes + referenced files in diff).
- `doc-substance-check` — **advisory** (`continue-on-error: true`).
  Predicate needs "is this pure refactor?" judgement.
- `adr-backfill-check` — **advisory** (`continue-on-error: true`).
  Predicate needs "is this decision non-trivial?" judgement.

Advisory/blocking split is load-bearing — see
[ADR-0124](../docs/adr/0124-automated-rule-enforcement.md) §Consequences
and VIF-fix false-positive in
[Research-0002](../docs/research/0002-automated-rule-enforcement.md)
§"Dead ends". Moving either advisory job into `required_status_checks`
(or flipping its `continue-on-error` flag) is policy change, needs
superseding ADR.

### Opt-out syntax parser

`deep-dive-checklist` job parses PR bodies for ADR-0108's
opt-out lines:

```text
no digest needed: <reason>
no alternatives: <reason>
no rebase-sensitive invariants
no reproducer needed: <reason>
no changelog needed: <reason>
no rebase impact: <reason>
```

Regex is intentionally loose on wording. If
[`PULL_REQUEST_TEMPLATE.md`](PULL_REQUEST_TEMPLATE.md) ever renames
six deliverables, parser's `key` mapping in
`rule-enforcement.yml` (step "Parse six-deliverable checklist")
must move in lockstep. Search for `case "${item}" in` block.

### Upstream-port exemption

`deep-dive-checklist` skips when PR title starts with `port:` /
`port(scope):` or branch name starts with `port/`. Those are
only two knobs; port PR using neither form WILL be
blocked. If sync skill
([`.claude/skills/port-upstream-commit/`](../.claude/skills/port-upstream-commit/))
ever changes its branch-naming or title convention, update
workflow's `Skip upstream-port PRs` step.

### ADR collision guard and stacked PRs

`adr-collision-check` job in
[`rule-enforcement.yml`](workflows/rule-enforcement.yml) scans open PRs for
ADR-number collisions, but must skip descendant stacked PRs whose
`baseRefName` chain reaches current PR's `head.ref`. Those descendants
intentionally contain parent PR's ADR files while merge train waits.
Never replace that base-branch-chain check with flat "any open PR with
same number fails" scan; it deadlocks ADR-bearing parent PRs whenever draft
children are queued.

Phase 1 compares added ADR numbers against PR's event `base.sha`, not
live `origin/master`. That distinction is load-bearing: if PR merges before
collision job starts, live `origin/master` already contains PR's own
ADR, produces false self-collision.

### Required aggregator and draft PRs

[`required-aggregator.yml`](workflows/required-aggregator.yml) is single
branch-protection status. It must run on draft PRs, fail them explicitly
instead of being skipped. Skipped required context is considered successful
by GitHub branch protection, so job-level draft skips can let auto-merge merge
PR before ready-for-review CI run registers. Aggregator also ignores
check runs older than its current workflow run when selecting sibling
outcomes; otherwise stale draft-era skipped check runs on same commit can
mask real queued or failed ready-for-review checks.

### Pelorus mirror verification stays in required Pre-Commit (ADR-1113, ADR-1276)

The `Pre-Commit` job in `lint-and-format.yml` resolves the full commit declared
by `scripts/sync-pelorus-interop.sh`, checks out `VMAFx/pelorus` at that exact
object with credentials disabled, and runs the default mirror/fixture drift
check. Keep this before `pre-commit --all-files`. Never change `ref` to a
moving branch or tag, duplicate the pin in workflow YAML, or tolerate a missing
object: ABI-stable parser safety releases must be able to trigger a reviewed
re-pin, and CI must prove source provenance rather than local-tree similarity.

### Go validation (ADR-1238)

`go-ci.yml` reports `go vet + go test` as required. It starts on non-draft
PRs including `ready_for_review`, master pushes, and manual dispatches,
then gates heavyweight steps on `go_checks` (`go` plus `c_core`). Preserve
its explicit documentation-only no-work result, CPU/optional-backend
settings, and CI-authority classification. Rules job runs
`scripts/ci/test_go_workflow_contract.py` before authoring exemptions;
this test executes aggregator script with failing Go outcomes.

### CI job display names and aggregator parity

All workflow job and matrix display names (`name:`) target $\le 30$ characters,
omit trailing policy citations and redundant parentheticals (see
[`docs/development/ci-job-names.md`](../docs/development/ci-job-names.md)).
Every required check declared in `required-aggregator.yml` (`const required = [...]`)
is tagged with `# required-aggregator` on its defining `name:` line in its workflow.
Script `scripts/ci/check-aggregator-names.sh` gates 1:1 parity between
`required-aggregator.yml` and workflow files; any rename or addition must
update both atomically.

### Advisory surface-path lists

Both advisory jobs grep diff for specific path prefixes
(`core/include/`, `meson_options.*`, `mcp-server/`, etc.).
These mirror
[ADR-0100](../docs/adr/0100-project-wide-doc-substance-rule.md) §Per-surface
and ADR-policy-surface list from
[ADR-0106](../docs/adr/0106-adr-maintenance-rule.md). When either
ADR adds new user-discoverable or policy-surface path, update
grep patterns in `rule-enforcement.yml` in same PR — otherwise
advisory goes silent on new surface.

### SHA-pin invariant for `uses:` directives

Every `uses:` directive in `.github/workflows/*.yml` MUST reference
40-char commit SHA, with original semver tag preserved as
trailing `# vN.M.K` comment. Floating-tag references (`@v4`,
`@release/v1`) trip OSSF Scorecard `Pinned-Dependencies` check,
rejected by sync gate below.

**Single permitted exception**:
`slsa-framework/slsa-github-generator/.github/workflows/generator_generic_slsa3.yml`
keeps its `vX.Y.Z` tag form because GitHub Actions consumers cannot
SHA-pin reusable-workflow refs in every code path; carve-out is
documented inline in
[`workflows/supply-chain.yml`](workflows/supply-chain.yml), and
mirrored in
[`docs/rebase-notes.md` entry 0231](../docs/rebase-notes.md).

**Sync gate** (run before merging any `/sync-upstream` that touches
`.github/workflows/`):

```bash
grep -hnE '^\s*(- )?uses:\s+[^@]+@[^ #]+\s*$' .github/workflows/*.yml \
  | grep -vE '@[a-f0-9]{40}' \
  | grep -v 'slsa-framework/slsa-github-generator/.github/workflows/'
# Empty output = clean. Anything that prints needs to be SHA-pinned
# before the sync PR can merge.
```

**Resolution recipe** when adding a new action or bumping an existing
pin:

```bash
# Lightweight tag (most actions):
gh api repos/<owner>/<repo>/git/ref/tags/<vN.M.K> --jq '.object.sha'
# Annotated tag (e.g. github/codeql-action, ilammy/msvc-dev-cmd,
# pypa/gh-action-pypi-publish) — first call returns
# `object.type == "tag"`; dereference it:
gh api repos/<owner>/<repo>/git/tags/<sha-from-prev> --jq '.object.sha'
```

See [ADR-1247](../docs/adr/1247-scorecard-exact-head-gates.md) for
current project-level Scorecard policy (superseding PR #337 / ADR-0263)
and entry 0231 of [`docs/rebase-notes.md`](../docs/rebase-notes.md) for standing
re-test command.

### Dependency-update bot: Renovate, not Dependabot (ADR-0363)

Fork uses **Mend Renovate** self-hosted via
[`workflows/renovate.yml`](workflows/renovate.yml). `.github/dependabot.yml`
has been removed, content archived as `.github/dependabot.yml.disabled`.

On upstream sync:

- If Netflix adds `dependabot.yml`, **never** restore it — merge content
  into `dependabot.yml.disabled` for reference only. Fork's dependency-update
  bot is Renovate; running both simultaneously causes duplicate PRs.
- `renovate.yml` and `renovate.json` are fork-local; Netflix upstream will never
  ship them. They are safe from upstream conflicts.
- `RENOVATE_TOKEN` is repository secret; not committed anywhere. Operator
  playbook is at
  [`docs/development/dependency-bot.md`](../docs/development/dependency-bot.md).

### Root Python requirement stays minor-series scoped

Root [`pyproject.toml`](../pyproject.toml) is tool-only metadata, keeps
`requires-python = ">=3.14"`, without patch component. Dependabot updater
images can lag newest CPython patch, so patch-specific floor makes
automatic pip dependency graph fail before it can inspect any package.
Exact-root `pep621` rule in [`renovate.json`](../renovate.json) disables updates
for that one `requires-python` entry. Keep workflow `setup-python` pins and real
package constraints independently managed; never broaden exclusion to
subdirectory `pyproject.toml` files.

### ONNX Runtime release version and digest stay coupled

Linux all-backends row in [`workflows/build.yml`](workflows/build.yml)
downloads ONNX Runtime into `RUNNER_TEMP`, verifies release asset's SHA-256,
only then extracts it with `sudo`. Keep `ORT_VERSION` and `ORT_SHA256`
updated together from official GitHub release asset metadata. Never pipe
retrying `curl` transfer directly into `tar`: retries require file-backed
output, and privileged extraction must not see unverified bytes.

### Helm workflow version and digest stay coupled

[`workflows/helm-chart.yml`](workflows/helm-chart.yml) and
[`workflows/e2e-k8s.yml`](workflows/e2e-k8s.yml) install same pinned Helm
archive from `get.helm.sh`. Keep `HELM_VERSION`, `HELM_SHA256`, and verified
file-backed extraction sequence identical in both workflows. Never restore
moving `helm/helm@main` installer or pipe network bytes into shell; update
digest from Helm's official checksum whenever version changes.

E2E workflow deliberately gives kuttl step `continue-on-error` so
diagnostics and XML can still upload. Its final assertion must inspect
`steps.kuttl.outcome` (not `conclusion`), fail unless it is `success`;
otherwise command failures are converted into green workflow.

E2E image build must explicitly select `target: node-cpu` for
`docker/Dockerfile.node` and `target: go-server` for `Dockerfile.go-server`.
`BACKEND=cpu` is not declared node-Dockerfile argument, cannot select
multi-stage target; without `target`, BuildKit chooses last stage
(`node-sycl`). Export and load operator, node, and server `e2e-test` tags as
one contract. Node image model copy must remain flat at configured
`VMAFX_MODEL_DIR`. Chart smoke sets both pull policies to `Never`, installs
default server Deployment on CPU, keeps operator out of
component-qualified scoring Service, performs real `/v1/score`; never
replace it with health-only or reconciler behavior that production code does
not implement. Keep workflow and `docs/k8s/integration-tests.md` aligned.
Dependency-free `scripts/ci/test_e2e_runtime_contract.py` runs both in
E2E image-build job and always-on `deep-dive-checklist` job in
`rule-enforcement.yml`; never move it solely behind E2E schedule/label
gate. Cluster job writes `VMAFX_E2E_KUBECONFIG` and `KUBECONFIG` to
same new file below `RUNNER_TEMP`; every Kubernetes step must first prove
exact `kind-${KIND_CLUSTER_NAME}` context and loopback API server. Teardown
must fail visibly if that identity guard cannot prove exact named cluster.

Security Scans concurrency group includes `github.event_name` between
workflow name and ref. Scheduled scan and master push both use
`refs/heads/master`; without event discriminator, either can cancel
other's CodeQL coverage. Preserve `cancel-in-progress: true` so superseded
runs of same event/ref still collapse, keep
`scripts/ci/test_security_workflow_contract.py` in always-on Rules gate.

## Sanitizer matrix test-set scope (ADR-0347)

`sanitizers` job in
[`workflows/tests-and-quality-gates.yml`](workflows/tests-and-quality-gates.yml)
enumerates full C unit-test set via `meson test --list`, applies
per-sanitizer regex deselect:

- `address` — excludes `test_model`, `test_predict`,
  `test_float_ms_ssim_min_dim`.
- `undefined` — excludes `test_model`. Build also adds
  `-Dc_args=-fno-sanitize=function` and `cpp_args` twin to
  suppress K&R-prototype harness UB across ~50 test files
  (`core/test/test.h` callers).
- `thread` — excludes `test_model`, `test_pic_preallocation`,
  `test_framesync`. Note: `test_thread_safety_batch` is TSan-eligible
  counterpart of `test_pic_preallocation` (covers same
  threaded_extract_batch_func paths via ADR-1072/ADR-1073 without
  vmaf_preallocate_pictures), is intentionally NOT excluded.

Every deselected entry corresponds to real defect tracked in
[`../docs/state.md`](../docs/state.md) Open-bugs. As fixes land,
corresponding `EXCLUDE='...'` regex shrinks. Never
silently widen deselect list to "make CI pass" — per
`feedback_no_test_weakening`, every addition needs ADR
referencing underlying bug. Reverting `--suite=unit` would
re-introduce zero-coverage gap (no `test()` call carries
`suite: 'unit'` tag in `core/test/meson.build`); workflow
must keep enumerating from `meson test --list`.

## Windows CUDA setup path (ADR-0664)

`libvmaf-build-matrix.yml` installs CUDA 13.3.1 directly in the
`Windows MSVC+CUDA` leg. Do not restore
`Jimver/cuda-toolkit` for that Windows leg without a superseding ADR
and a green required Windows CUDA run: v0.2.35 failed before setup on
PR #1463, blocked merge train without Meson or compiler output.

Linux CUDA legs still use `Jimver/cuda-toolkit`; ADR-0664 only
special-cases Windows network-installer path. Keep explicit
Windows package set (`nvcc`, `cudart`, `crt`, `nvvm`, and
`visual_studio_integration`) aligned with CUDA major/minor suffix
in workflow when bumping CUDA.

## Windows ARM64 lane (ADR-1260)

`windows-arm64` job in `libvmaf-build-matrix.yml`, display name
`Windows ARM64 MSVC`, advisory: no `# required-aggregator` marker, not in
`required-aggregator.yml`. Promotion = ADR amendment + aggregator entry in
same PR, maintainer decision.

Invariants:

- `runs-on: windows-11-vs2026-arm`. `windows-11-arm` migrates to same VS 2026
  image 2026-09-21..30 (runner-images #14602); switch only after that, and
  only with a green run.
- `setup-msvc-dev` `arch: arm64` = `vcvarsall.bat arm64`, ARM64-hosted native
  toolset. Never `amd64_arm64` (x64 cross compiler under emulation). `Show
  compiler` step greps `cl.exe` banner for `for ARM64`; keep it, fails fast on
  wrong host toolset.
- PE machine check (`0xAA64`) on `install\bin\vmaf.exe` stays: x64 binary
  runs under emulation and would pass tests.
- Python pin = `PYTHON_CI_VERSION` like x64 legs; `actions/python-versions`
  has `win32/arm64` for it. `pip install meson ninja`: ninja ships
  `win_arm64` wheel. No nasm step; x86-only probe in `core/src/meson.build`.
- CPU only until CUDA 13.4 bump: 13.3.1 has no `windows-arm64` packages.
- Test step = `meson test --suite fast --print-errorlogs`. Full suite =
  follow-up, own decision.

## Upstream-merge guidance

Netflix/vmaf ships its own workflows under `.github/workflows/`
(CI, release, etc.). Fork's workflows live alongside them; file
collisions are rare because fork-added workflow names
(`rule-enforcement.yml`, `nightly-bisect.yml`, `supply-chain.yml`,
`renovate.yml`, etc.) don't clash with upstream's names. On sync:

1. Preserve every fork-added workflow verbatim unless ADR that
   introduced it is superseded.
2. For workflows existing in both trees (e.g. `codeql.yml`),
   prefer fork version — usually has stricter pins and
   broader matrix legs.
3. `PULL_REQUEST_TEMPLATE.md` is fork-authored; upstream has none.
   Never overwrite it on sync.

## Signing + attestation chain invariants (ADR-0902)

Container builds in `.github/workflows/docker-publish-production.yml` and
`.github/workflows/docker-publish-operator-node.yml`, plus release-blob signing
in `.github/workflows/supply-chain.yml`, carry multi-layer signing chain
load-bearing for
[release.md](../docs/development/release.md) consumer verification recipes.

- Every container build job (CPU, CUDA, ROCm, oneAPI, Python MCP server, Go
  scoring server, operator, node)
  must run
  **both** `cosign sign --yes` **and** `actions/attest-build-provenance@<v4>`
  against same `${{ steps.push.outputs.digest }}`. Two
  attestations cover different consumer toolchains (cosign for
  Sigstore-native consumers, `gh attestation verify` for GitHub-native
  consumers); neither replaces other. Removing either side is
  policy change, needs superseding ADR.
- Both Docker workflows start with tag-bound validation job. Release or
  manual recovery must identify same published, non-prerelease ordinary
  SemVer tag through input, `GITHUB_REF`, `GITHUB_SHA`, checkout, and
  coordinated version files. Every image build needs that validation job,
  checks out its tag output; never restore branch-ref checkout with
  independently supplied publish tag.
- Every job running `actions/attest-build-provenance@*` needs
  `attestations: write` in its `permissions:` block. Adding new GPU
  variant without this permission silently disables GitHub-native
  attestation for that variant.
- Every container build job must generate CycloneDX SBOM with syft, attach
  it to same digest with `cosign attest`, upload JSON artifact.
  These steps are release gates: never add `continue-on-error` or other
  best-effort handling. Workflow summary must require every build job to
  finish with `success`; skipped GPU or server build is not accepted
  release result.
- Each Docker workflow's smoke job must run `cosign verify` against every
  freshly-pushed image it consumes before pulling and running it. Skipping
  this verification
  would re-open gap that ADR-0902 §G3 closed (compromised CI token
  pushes unsigned image; smoke test passes).
- Production GPU smoke consumes digest output from all three vendor
  build jobs, verifies each signature, then runs driver-independent
  `--version` entrypoint. Keep it in summary gate; GPU hardware is not
  required to catch broken runtime dependency closure.
- Certificate-identity regex in
  [`release.md`](../docs/development/release.md) §"Consumer verification
  recipes" assumes workflow file path
  `.github/workflows/docker-publish-production.yml` and
  `.github/workflows/docker-publish-operator-node.yml` and
  `.github/workflows/supply-chain.yml`. Renaming or splitting these
  workflows requires updating both docs AND any cached consumer
  scripts (deprecated regex stays valid for old image digests in Rekor).
- `cosign-installer` SHA-pin: every install step uses same pinned v4 SHA.
  When Renovate or manual bump updates it, every build/smoke job in both
  Docker workflows and both `supply-chain.yml` jobs must move together.
  Mixed-version chain produces signature-format mismatches, surfacing
  only at consumer-verify time.

## OSSF Scorecard pin invariant

`.github/workflows/scorecard.yml` references
`github/codeql-action/upload-sarif@<sha>`. Preserve resolving immutable
upstream commit and publishing restrictions of pinned Scorecard action.
Historical unresolved-pin failure is recorded in Research-0053; it does not
establish that later tag move invalidates otherwise valid commit. Whenever
this pin is updated (Renovate or manual), verify new SHA resolves:

```bash
pin=$(grep -oE 'codeql-action/upload-sarif@[a-f0-9]{40}' \
      .github/workflows/scorecard.yml | head -1 | cut -d@ -f2)
gh api "/repos/github/codeql-action/commits/$pin" --jq '.sha'
```

A 422 response here is canary that workflow is about to start
failing on next push. See [ADR-1247](../docs/adr/1247-scorecard-exact-head-gates.md)
and [Research-0053](../docs/research/0053-ossf-scorecard-investigation.md).

## macOS tmate SSH debug step (ADR-0626)

`libvmaf-build-matrix.yml` carries SSH debug step after `Run tests`
step in `libvmaf-build` job:

```yaml
- name: SSH debug session on test failure
  if: ${{ failure() && runner.os == 'macOS' && github.event_name == 'workflow_dispatch' }}
  uses: mxschmitt/action-tmate@c0afd6f790e3a5564914980036ebf83216678101  # v3
```

Rebase-sensitive invariants:

- `if:` triple condition load-bearing. **All three clauses preserved
  together.** Dropping `github.event_name == 'workflow_dispatch'` causes step
  to open blocking SSH session on every failing PR push. Strands macOS runner
  up to 30 minutes per failure.
- Step stays **after** `Run tests` step -> fires only when test failure already
  set job status to `failure()`.
- Action pinned to commit SHA per fork's Renovate
  `helpers:pinGitHubActionDigests` policy. Renovate will propose digest bumps;
  accept only after verifying new SHA corresponds to signed release tag.
- Step is intentionally present in shared matrix job (not separate
  macOS-only job) because `runner.os == 'macOS'` clause in `if:`
  already restricts it to macOS legs. Never split it into separate job.

See [ADR-0626](../docs/adr/0626-macos-ci-tmate-debug-on-failure.md) and
[`docs/development/ci-tmate-debug.md`](../docs/development/ci-tmate-debug.md).

## Build matrix of record (ADR-1259)

[ADR-1259](../docs/adr/1259-ci-build-matrix-as-it-runs.md) lists every lane in
`libvmaf-build-matrix.yml` and `build.yml` and which ones are required.
ADR-0689, ADR-0691, ADR-0710 and ADR-0728 are superseded: do not remove a lane
on their authority, and do not let a merge resolution drop or restore a lane
without an ADR. That is how `384d97d03` undid two of them.

The MoltenVK lane (ADR-0338) went with the Vulkan backend (ADR-0726). The
`libvmaf-build` job's `continue-on-error` is now
`${{ matrix.experimental == true }}`, so the two `experimental: true` rows,
`macOS clang` and `macOS clang+DNN`, are advisory: their failure does not
fail the workflow run. Neither is a required check.

## Renovate (ADR-0363) supersedes Dependabot

Note: pin updates to `codeql-action/upload-sarif` now arrive via Renovate
(grouped with other GitHub Actions minor+patch bumps), not Dependabot.

## Related

- [ADR-0124](../docs/adr/0124-automated-rule-enforcement.md) — this tooling
- [ADR-1247](../docs/adr/1247-scorecard-exact-head-gates.md) — current OSSF
  Scorecard policy; ADR-0263 is superseded
- [ADR-0338](../docs/adr/0338-macos-vulkan-via-moltenvk-lane.md) — macOS
  Vulkan-via-MoltenVK advisory lane (removed with the Vulkan backend, ADR-0726)
- [ADR-1259](../docs/adr/1259-ci-build-matrix-as-it-runs.md) — the CI build
  matrix as it runs
- [Research-0002](../docs/research/0002-automated-rule-enforcement.md) — investigation
- [Research-0053](../docs/research/0053-ossf-scorecard-investigation.md) —
  OSSF Scorecard per-check breakdown
- [Research-0089](../docs/research/0089-moltenvk-feasibility-on-fork-shaders.md)
  — MoltenVK feasibility against fork's shader inventory
- [`docs/development/automated-rule-enforcement.md`](../docs/development/automated-rule-enforcement.md)
  — user-facing explainer
- [`docs/rebase-notes.md` entry 0026](../docs/rebase-notes.md) — sync ledger
- [ADR-0363](../docs/adr/0363-renovate-replaces-dependabot.md) —
  Renovate replaces Dependabot
- [`docs/development/dependency-bot.md`](../docs/development/dependency-bot.md)
  — operator playbook

## `clang-tidy-<N>` always needs the apt.llvm.org repo

Ubuntu 24.04 (`ubuntu-latest` / `ubuntu-24.04`) ships **clang-tidy-18** at
most. Every job installing newer `clang-tidy-<N>` must add LLVM
archive first:

```yaml
wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
chmod +x /tmp/llvm.sh
sudo /tmp/llvm.sh 22
sudo apt-get install -y clang-tidy-22
```

Listing `clang-tidy-22` in plain `apt-get install` line aborts whole
step with `E: Unable to locate package clang-tidy-22`.

**This failure hides itself.** These jobs are gated on
`if: steps.detect.outputs.files != ''`, so on any PR changing no file in
scope every step is skipped and job reports **success**. Broken apt
line is only reached when job has real work, so run history looks
mostly green while gate has never once executed. `Clang-Tidy SYCL (Changed
Files, Advisory)` sat in exactly that state from LLVM 22 bump
(PR #1161, PR #1200) until it was fixed: 7 green no-op runs, 2 red runs,
zero SYCL files ever linted.

When bumping clang-tidy major, grep workflow for **every**
`clang-tidy-<old>` occurrence, confirm each one is preceded by
`llvm.sh` step. Verify job's green run did real work before
trusting it.

## `cc.find_library('foo')` needs the `-dev` package, not the runtime one

meson's `cc.find_library('foo')` emits literal `-lfoo`. `ld` resolves `-lfoo`
against `libfoo.so` or `libfoo.a` **only** — unversioned linker symlink
that lives in `-dev` package. Versioned runtime SONAME `libfoo.so.1`
that runtime package ships is invisible to `-l`, so installing runtime
package alone leaves probe failing with
`/usr/bin/ld: cannot find -lfoo` and meson's
`ERROR: C shared or static library 'foo' not found`.

Concretely, for Level Zero loader on `ubuntu-24.04`:

```yaml
sudo apt-get install -y libze-dev   # libze_loader.so + level_zero/ze_api.h
# NOT libze1 — that ships only libze_loader.so.1
```

Two traps around this one:

- **oneAPI does not supply it.** `intel-oneapi-compiler-dpcpp-cpp` plus
  `source /opt/intel/oneapi/setvars.sh --force` still leaves `-lze_loader`
  unresolvable. Loader is separate, vendor-neutral dispatch library.
- **Package name is release-specific.** It is `libze-dev` on 24.04
  `noble` (source package `level-zero`, in `universe`, already enabled on
  hosted image). `level-zero-dev` does **not** exist on noble; never copy
  that name from newer release or from comment written for one.

Loader links with no GPU present, never calls `zeInit`, so no
accelerator, no `intel-level-zero-gpu`, and no device-plugin resource is
needed for configure/compile lane. Never reach for Intel's graphics APT
repository to satisfy `-lze_loader`; Intel's oneAPI APT repository contains no
`level-zero` packages at all.

`.github/workflows/libvmaf-build-matrix.yml` solves same requirement
differently — it builds `oneapi-src/level-zero` from source at pinned tag,
because it links shipping artifact, wants known loader version. A
static-analysis lane needing only probe to resolve should prefer
distro package.

## SYCL build trees on hosted runners must disable LTO

`core/meson.build` sets `b_lto=true` in project's `default_options`, so any
`meson setup` not overriding it links with `-flto`. On
GitHub-hosted `ubuntu-24.04` runner, this routes LTO through stock binutils
LLVM gold plugin, which is **LLVM 17.0.6**. It cannot read bitcode emitted by
oneAPI DPC++ compiler, and every binary fails to link:

```text
bfd plugin: LLVM gold plugin has failed to create LTO module:
Unknown attribute kind (102)
(Producer: 'Intel.oneAPI.DPCPP.Compiler_2026.1.1' Reader: 'LLVM 17.0.6')
```

Pass `-Db_lto=false` on every icpx/SYCL `meson setup` in CI. Both SYCL legs of
`libvmaf-build-matrix.yml` already do, as do `build.yml`'s `Linux Intel LLVM`
row and the `Tidy SYCL (advisory)` job. Pinning an older oneAPI does not
help — the mismatch is against the *system* linker plugin, not a specific
compiler release.

## Scorecard scope and report authenticity (ADR-1247)

Keep `scorecard.yml` publisher restricted to upstream-approved actions;
only that job may obtain OIDC write permission. Its generated JSON and SARIF
artifact name binds run ID, attempt and event SHA. Separate Master Gate
requires successful analysis, downloads only same run's artifact; never
replace it with public latest-score API. Action scans remote HEAD, so
mismatch with event SHA must fail. Keep PR workflow read-only and
non-publishing, with exact head checkout and source snapshots around all eleven
local file checks. Applicable Scorecard context must report success in
aggregator; absent, skipped and neutral are failures. Inactive event's gate
must not create pre-merge wait for future master result. Contract tests are
`scripts/ci/tests/test_scorecard_*.py`; preserve pinned source/schema checks
and exact no-release-only unavailable case. Both gates and local
`repository-security-contract` hook must run offline ADR-1248 controls.
Master gate runs read-only repository security checker outside
publisher, using built-in token, retaining separate live receipt;
failed Scorecard assessment must not silently skip that drift check.
