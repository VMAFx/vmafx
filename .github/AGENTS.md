# Agent notes — `.github/` (workflows + templates)

Parent: [../AGENTS.md](../AGENTS.md).

This directory holds GitHub-facing config: Actions workflows, issue /
PR templates, the CODEOWNERS file. Everything here is fork-local —
Netflix/vmaf upstream has its own `.github/` that rarely overlaps
path-wise, so conflicts on merge tend to be rare but high-impact
when they happen (a silently-broken workflow is less visible than a
broken `.c` file).

## Invariants a reviewer or sync must preserve

### Single SemVer release fan-out (ADR-1127)

`release-please.yml` owns one root package and creates a draft GitHub release.
Publishing that draft is the authenticated operation that creates the `vX.Y.Z`
tag and starts `supply-chain.yml` plus both Docker publication workflows. Do not
split release-please back into unqualified component tags or make the release
non-draft without first providing and validating an explicit downstream
workflow trigger. The workflow pauses both release-please phases while one
ordinary-SemVer draft exists; later master pushes must not move or duplicate a
release waiting at the human publication gate. The initial 3.2.1 cut is selected
by a one-time `release-as` config field; release-PR rollover must remove that
field and `bootstrap-sha` before the release PR merges so neither override can
affect 3.2.2.

`supply-chain.yml` runs `scripts/release/verify-release-version.sh` before any
write or OIDC job. It keeps native and `vmaf-mcp` hashes in distinct SLSA jobs
with distinct provenance asset names. Its native SBOMs contain and hash every
staged artifact; its Python SBOMs inventory the installed `vmaf-mcp` dependency
graph plus the wheel and sdist. The workflow fails if either inventory becomes
empty or mislabeled. Anchore's implicit artifact/release uploads stay disabled:
the explicit SBOM artifact feeds keyless signing before the final strict
attachment job. Do not bypass that DAG or restore permissive unmatched-file
uploads; a green workflow must mean every promised asset exists.

Manual supply-chain recovery must use the published tag as both the workflow ref
and input (`gh workflow run supply-chain.yml --ref "$tag" -f tag="$tag"`). The
validation job rejects branch-ref dispatches, missing/draft/prerelease releases,
or an event SHA different from the checked-out tag. This binding is required so
SLSA provenance describes the source that produced the artifacts.

### Rule-enforcement split (ADR-0124)

[`rule-enforcement.yml`](workflows/rule-enforcement.yml) has three
jobs. Only **one** is allowed to be required-status-check-blocking:

- `deep-dive-checklist` — **blocking**. Predicate is mechanically
  decidable (ticked checkboxes + referenced files in diff).
- `doc-substance-check` — **advisory** (`continue-on-error: true`).
  Predicate needs "is this a pure refactor?" judgement.
- `adr-backfill-check` — **advisory** (`continue-on-error: true`).
  Predicate needs "is this decision non-trivial?" judgement.

The advisory/blocking split is load-bearing — see
[ADR-0124](../docs/adr/0124-automated-rule-enforcement.md) §Consequences
and the VIF-fix false-positive in
[Research-0002](../docs/research/0002-automated-rule-enforcement.md)
§"Dead ends". Moving either advisory job into `required_status_checks`
(or flipping its `continue-on-error` flag) is a policy change and
needs a superseding ADR.

### Opt-out syntax parser

The `deep-dive-checklist` job parses PR bodies for ADR-0108's
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
the six deliverables, the parser's `key` mapping in
`rule-enforcement.yml` (step "Parse six-deliverable checklist")
must move in lockstep. Search for the `case "${item}" in` block.

### Upstream-port exemption

`deep-dive-checklist` skips when the PR title starts with `port:` /
`port(scope):` or the branch name starts with `port/`. Those are
the only two knobs; a port PR that uses neither form WILL be
blocked. If the sync skill
([`.claude/skills/port-upstream-commit/`](../.claude/skills/port-upstream-commit/))
ever changes its branch-naming or title convention, update the
workflow's `Skip upstream-port PRs` step.

### ADR collision guard and stacked PRs

The `adr-collision-check` job in
[`rule-enforcement.yml`](workflows/rule-enforcement.yml) scans open PRs for
ADR-number collisions, but it must skip descendant stacked PRs whose
`baseRefName` chain reaches the current PR's `head.ref`. Those descendants
intentionally contain the parent PR's ADR files while the merge train waits.
Do not replace that base-branch-chain check with a flat "any open PR with the
same number fails" scan; it deadlocks ADR-bearing parent PRs whenever draft
children are queued.

Phase 1 compares added ADR numbers against the PR's event `base.sha`, not
live `origin/master`. That distinction is load-bearing: if a PR merges before
the collision job starts, live `origin/master` already contains the PR's own
ADR and produces a false self-collision.

### Required aggregator and draft PRs

[`required-aggregator.yml`](workflows/required-aggregator.yml) is the single
branch-protection status. It must run on draft PRs and fail them explicitly
instead of being skipped. A skipped required context is considered successful
by GitHub branch protection, so job-level draft skips can let auto-merge merge
a PR before the ready-for-review CI run registers. The aggregator also ignores
check runs older than its current workflow run when selecting sibling
outcomes; otherwise stale draft-era skipped check runs on the same commit can
mask real queued or failed ready-for-review checks.

### Advisory surface-path lists

Both advisory jobs grep the diff for specific path prefixes
(`core/include/`, `meson_options.*`, `mcp-server/`, etc.).
These mirror
[ADR-0100](../docs/adr/0100-project-wide-doc-substance-rule.md) §Per-surface
and the ADR-policy-surface list from
[ADR-0106](../docs/adr/0106-adr-maintenance-rule.md). When either
ADR adds a new user-discoverable or policy-surface path, update the
grep patterns in `rule-enforcement.yml` in the same PR — otherwise
the advisory goes silent on the new surface.

### SHA-pin invariant for `uses:` directives

Every `uses:` directive in `.github/workflows/*.yml` MUST reference a
40-char commit SHA, with the original semver tag preserved as a
trailing `# vN.M.K` comment. Floating-tag references (`@v4`,
`@release/v1`) trip the OSSF Scorecard `Pinned-Dependencies` check
and are rejected by the sync gate below.

**Single permitted exception**:
`slsa-framework/slsa-github-generator/.github/workflows/generator_generic_slsa3.yml`
keeps its `vX.Y.Z` tag form because GitHub Actions consumers cannot
SHA-pin reusable-workflow refs in every code path; the carve-out is
documented inline in
[`workflows/supply-chain.yml`](workflows/supply-chain.yml) and
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

See [ADR-0263](../docs/adr/0263-ossf-scorecard-policy.md) for the
project-level Scorecard policy (introduced by PR #337) and entry 0231
of [`docs/rebase-notes.md`](../docs/rebase-notes.md) for the standing
re-test command.

### Dependency-update bot: Renovate, not Dependabot (ADR-0363)

The fork uses **Mend Renovate** self-hosted via
[`workflows/renovate.yml`](workflows/renovate.yml). `.github/dependabot.yml`
has been removed and its content archived as `.github/dependabot.yml.disabled`.

On upstream sync:

- If Netflix adds a `dependabot.yml`, do **not** restore it — merge the content
  into `dependabot.yml.disabled` for reference only. The fork's dependency-update
  bot is Renovate; running both simultaneously causes duplicate PRs.
- `renovate.yml` and `renovate.json` are fork-local; Netflix upstream will never
  ship them. They are safe from upstream conflicts.
- `RENOVATE_TOKEN` is a repository secret; it is not committed anywhere. The
  operator playbook is at
  [`docs/development/dependency-bot.md`](../docs/development/dependency-bot.md).

## Sanitizer matrix test-set scope (ADR-0347)

The `sanitizers` job in
[`workflows/tests-and-quality-gates.yml`](workflows/tests-and-quality-gates.yml)
enumerates the full C unit-test set via `meson test --list` and
applies a per-sanitizer regex deselect:

- `address` — excludes `test_model`, `test_predict`,
  `test_float_ms_ssim_min_dim`.
- `undefined` — excludes `test_model`. Build also adds
  `-Dc_args=-fno-sanitize=function` and the `cpp_args` twin to
  suppress the K&R-prototype harness UB across ~50 test files
  (`core/test/test.h` callers).
- `thread` — excludes `test_model`, `test_pic_preallocation`,
  `test_framesync`. Note: `test_thread_safety_batch` is the TSan-eligible
  counterpart of `test_pic_preallocation` (covers the same
  threaded_extract_batch_func paths via ADR-1072/ADR-1073 without
  vmaf_preallocate_pictures) and is intentionally NOT excluded.

Every deselected entry corresponds to a real defect tracked in
[`../docs/state.md`](../docs/state.md) Open-bugs. As fixes land
the corresponding `EXCLUDE='...'` regex shrinks. Do **not**
silently widen the deselect list to "make CI pass" — per
`feedback_no_test_weakening`, every addition needs an ADR
referencing the underlying bug. Reverting `--suite=unit` would
re-introduce the zero-coverage gap (no `test()` call carries a
`suite: 'unit'` tag in `core/test/meson.build`); the workflow
must keep enumerating from `meson test --list`.

## Windows CUDA setup path (ADR-0664)

`libvmaf-build-matrix.yml` installs CUDA 13.2.0 directly in the
`Build — Windows MSVC + CUDA (build only)` leg. Do not restore
`Jimver/cuda-toolkit` for that Windows leg without a superseding ADR
and a green required Windows CUDA run: v0.2.35 failed before setup on
PR #1463 and blocked the merge train without Meson or compiler output.

The Linux CUDA legs still use `Jimver/cuda-toolkit`; ADR-0664 only
special-cases the Windows network-installer path. Keep the explicit
Windows package set (`nvcc`, `cudart`, `crt`, `nvvm`, and
`visual_studio_integration`) aligned with the CUDA major/minor suffix
in the workflow when bumping CUDA.

## Upstream-merge guidance

Netflix/vmaf ships its own workflows under `.github/workflows/`
(CI, release, etc.). The fork's workflows live alongside them; file
collisions are rare because the fork-added workflow names
(`rule-enforcement.yml`, `nightly-bisect.yml`, `supply-chain.yml`,
`renovate.yml`, etc.) don't clash with upstream's names. On sync:

1. Preserve every fork-added workflow verbatim unless the ADR that
   introduced it is superseded.
2. For workflows that exist in both trees (e.g. `codeql.yml`),
   prefer the fork version — it usually has stricter pins and
   broader matrix legs.
3. `PULL_REQUEST_TEMPLATE.md` is fork-authored; upstream has none.
   Never overwrite it on sync.

## Signing + attestation chain invariants (ADR-0902)

Container builds in `.github/workflows/docker-publish-production.yml` and
release-blob signing in `.github/workflows/supply-chain.yml` carry a
multi-layer signing chain that is load-bearing for the
[release.md](../docs/development/release.md) consumer verification recipes.

- Every container build job (CPU, CUDA, ROCm, oneAPI, server) must run
  **both** `cosign sign --yes` **and** `actions/attest-build-provenance@<v4>`
  against the same `${{ steps.push.outputs.digest }}`. The two
  attestations cover different consumer toolchains (cosign for
  Sigstore-native consumers, `gh attestation verify` for GitHub-native
  consumers); neither replaces the other. Removing either side is a
  policy change and needs a superseding ADR.
- Every job that runs `actions/attest-build-provenance@*` needs
  `attestations: write` in its `permissions:` block. Adding a new GPU
  variant without this permission silently disables the GitHub-native
  attestation for that variant.
- The `smoke-test` job must run `cosign verify` against the freshly-pushed
  CPU image before pulling and running it. Skipping this verification
  would re-open the gap that ADR-0902 §G3 closed (compromised CI token
  pushes an unsigned image; smoke test passes).
- The certificate-identity regex in
  [`release.md`](../docs/development/release.md) §"Consumer verification
  recipes" assumes the workflow file path
  `.github/workflows/docker-publish-production.yml` and
  `.github/workflows/supply-chain.yml`. Renaming or splitting these
  workflows requires updating both the docs AND any cached consumer
  scripts (deprecated regex stays valid for old image digests in Rekor).
- `cosign-installer` SHA-pin: every install step uses the same pinned
  v4 SHA. When Renovate or a manual bump updates it, all five Docker
  build jobs + the smoke-test job + both supply-chain.yml jobs must
  move together — a mixed-version chain produces signature-format
  mismatches that only surface at consumer-verify time.

## OSSF Scorecard pin invariant

`.github/workflows/scorecard.yml` references
`github/codeql-action/upload-sarif@<sha>`. The Scorecard webapp at
`api.scorecard.dev` validates the pinned SHA against the action's
upstream repository on every publish; a SHA that no longer exists
under the declared tag (e.g. because upstream rewrote a release
branch or moved a tag) is rejected as an "imposter commit", returning
HTTP 400 and turning the workflow red. Whenever this pin is updated
(Renovate or manual), spot-check that the new SHA still resolves:

```bash
pin=$(grep -oE 'codeql-action/upload-sarif@[a-f0-9]{40}' \
      .github/workflows/scorecard.yml | head -1 | cut -d@ -f2)
gh api "/repos/github/codeql-action/commits/$pin" --jq '.sha'
```

A 422 response here is the canary that the workflow is about to start
failing on the next push. See [ADR-0263](../docs/adr/0263-ossf-scorecard-policy.md)
and [Research-0053](../docs/research/0053-ossf-scorecard-investigation.md).

## macOS tmate SSH debug step (ADR-0626)

`libvmaf-build-matrix.yml` carries an SSH debug step after the `Run tests`
step in the `libvmaf-build` job:

```yaml
- name: SSH debug session on test failure
  if: ${{ failure() && runner.os == 'macOS' && github.event_name == 'workflow_dispatch' }}
  uses: mxschmitt/action-tmate@c0afd6f790e3a5564914980036ebf83216678101  # v3
```

Rebase-sensitive invariants:

- The `if:` triple condition is load-bearing. **All three clauses must be
  preserved together.** Dropping `github.event_name == 'workflow_dispatch'`
  would cause the step to open a blocking SSH session on every failing PR push,
  stranding a macOS runner for up to 30 minutes per failure.
- The step must remain **after** the `Run tests` step and **before** the
  `Run Vulkan smoke tests (macOS MoltenVK)` step so it fires only when a
  test failure has already set the job status to `failure()`.
- The action is pinned to a commit SHA per the fork's Renovate
  `helpers:pinGitHubActionDigests` policy. Renovate will propose digest bumps;
  accept only after verifying the new SHA corresponds to a signed release tag.
- The step is intentionally present in the shared matrix job (not a separate
  macOS-only job) because the `runner.os == 'macOS'` clause in the `if:`
  already restricts it to macOS legs. Do not split it into a separate job.

See [ADR-0626](../docs/adr/0626-macos-ci-tmate-debug-on-failure.md) and
[`docs/development/ci-tmate-debug.md`](../docs/development/ci-tmate-debug.md).

## macOS Vulkan-via-MoltenVK lane (ADR-0338)

`libvmaf-build-matrix.yml` carries an advisory lane
`Build — macOS Vulkan via MoltenVK (advisory)` that runs on
`macos-latest` (Apple Silicon). Rebase-sensitive invariants:

- The lane is gated `continue-on-error: ${{ matrix.experimental ==
  true && matrix.moltenvk == true }}`. The compound predicate is
  load-bearing — the matrix has other `experimental: true` rows
  (the macOS DNN lane) that must keep their default fail-fast
  behaviour. A naive simplification to `${{ matrix.experimental }}`
  would silently make those other rows advisory.
- `VK_ICD_FILENAMES` MUST point at
  `/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json` — the homebrew
  formula `molten-vk` lays the JSON under `etc/vulkan/`, NOT
  `share/vulkan/`. Do not "fix" the path; verify against
  `Formula/m/molten-vk.rb` if in doubt.
- The lane must NOT be added to `required-aggregator.yml` until one
  green run lands on `master`. See ADR-0338 §Decision.
- The existing `Run tests` / cache / tox steps gate on
  `!matrix.moltenvk` — the moltenvk lane runs its own dedicated
  Vulkan-only smoke step. Do not unify or the lane will try to run
  tox tests against an Apple-Vulkan build, which is not the lane's
  contract.

See [ADR-0338](../docs/adr/0338-macos-vulkan-via-moltenvk-lane.md)
and [`docs/backends/vulkan/moltenvk.md`](../docs/backends/vulkan/moltenvk.md).

## Renovate (ADR-0363) supersedes Dependabot

Note: pin updates to `codeql-action/upload-sarif` now arrive via Renovate
(grouped with other GitHub Actions minor+patch bumps), not Dependabot.

## Related

- [ADR-0124](../docs/adr/0124-automated-rule-enforcement.md) — this tooling
- [ADR-0263](../docs/adr/0263-ossf-scorecard-policy.md) — OSSF Scorecard
  policy + accepted blockers
- [ADR-0338](../docs/adr/0338-macos-vulkan-via-moltenvk-lane.md) — macOS
  Vulkan-via-MoltenVK advisory lane
- [Research-0002](../docs/research/0002-automated-rule-enforcement.md) — investigation
- [Research-0053](../docs/research/0053-ossf-scorecard-investigation.md) —
  OSSF Scorecard per-check breakdown
- [Research-0089](../docs/research/0089-moltenvk-feasibility-on-fork-shaders.md)
  — MoltenVK feasibility against the fork's shader inventory
- [`docs/development/automated-rule-enforcement.md`](../docs/development/automated-rule-enforcement.md)
  — user-facing explainer
- [`docs/rebase-notes.md` entry 0026](../docs/rebase-notes.md) — sync ledger
- [ADR-0363](../docs/adr/0363-renovate-replaces-dependabot.md) —
  Renovate replaces Dependabot
- [`docs/development/dependency-bot.md`](../docs/development/dependency-bot.md)
  — operator playbook
