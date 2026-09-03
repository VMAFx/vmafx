<!-- markdownlint-disable -->
<!--
Research digest backing ADR-1151 (docs/adr/1151-vmafx-first-release-1-0-0.md).
Verbatim copy of the read-only audit run on 2026-09-03 against f7e56b333.
Preserved as-is: line/finding citations are the audit trail for the repair PR.
-->
# VMAFx/vmafx — release-please / release-pipeline audit

- **Audited tree**: `/home/kilian/dev/vmaf` @ `f7e56b333` (== `origin/master`)
- **Date**: 2026-09-03
- **Mode**: read-only. No tag, no release, no push, no merge. One release-please
  **dry run** only (`--dry-run`, exit 0, terminal action `Would open 1 pull requests`).
- **Binding maintainer decisions applied** (not re-litigated):
  1. The fork's first release is **1.0.0** on a fresh VMAFx number line. ADR-1127's
     "start at v3.2.1" is superseded.
  2. The cut happens **later**, after the fork's models are retrained for Netflix
     VMAF v1.0.16. The pipeline must be correct **and idle**.
  3. PR #1175 is already **CLOSED** by the maintainer (verified: `state: CLOSED`).
     release-please will open a fresh PR when master next moves; that PR must read
     **1.0.0**.

## Summary

The pipeline is *not* releasable today, and the reasons are structural rather than
cosmetic. Four issues are P0.

1. **No release PR can ever merge.** `release-please.yml` authenticates with
   `secrets.GITHUB_TOKEN`, so GitHub suppresses follow-on workflow events on the
   release branch. Every run on that branch is `action_required` with zero jobs;
   the sole required context (`Required Checks Aggregator`) therefore never
   reports, and the PR sits `BLOCKED` forever. This is repo-wide unique to the
   release branch — Renovate (App token) runs normally.
2. **The config still forces 3.2.1.** `release-as: "3.2.1"` + manifest `3.2.0` +
   ten markers at `3.2.1`. The dry run emits `chore(master): release 3.2.1`. The
   next push to master reopens a 3.2.1 PR, contradicting decision (1).
3. **Nothing proves the changelog cut ran.** No workflow invokes the rollover;
   `verify-release-version.sh` never mentions CHANGELOG; `concat --check` passes
   identically before and after a cut. `CHANGELOG.md` has exactly one `^## [`
   heading (`## [Unreleased]`, line 6), 1,550 fragments are live, and
   `changelog.d/releases/` does not exist.
4. **The publication environment does not exist.** Twelve write-bearing jobs
   declare `environment: release-publish`; the API returns 404, so GitHub would
   auto-create it with **no** protection rules. `pypi-publish` exists with
   `protection_rules: []`. `docs/development/release.md:56` asserts the opposite.

The signing/SBOM/SLSA chain itself, the draft-release human gate, the two-phase
release-please split, and the ten coordinated version markers are all sound — see
Verified-OK.

## Findings

Severity: **P0** = the next release would be wrong, impossible, or unsafe.
**P1** = breaks the release after next, or silently drifts. **P2/P3** = hygiene.

| # | Sev | Finding | Evidence (re-verified in this tree) | Fix |
|---|-----|---------|-------------------------------------|-----|
| 1 | P0 | Release PRs get **zero check runs**, so the one required context can never report and no release PR is mergeable | `release-please.yml:72,83` → `token: ${{ secrets.GITHUB_TOKEN }}`. `gh api repos/VMAFx/vmafx/actions/secrets` → `total_count: 0` (no App creds exist). Last 150 runs: `action_required` occurs on exactly one branch — `release-please--branches--master--components--vmafx`, 10 runs, 0 jobs each. `gh api .../branches/master/protection` → `contexts: ["Required Checks Aggregator"]`. PR #1175 was `mergeStateStatus: BLOCKED`. | Mint a GitHub App installation token (`actions/create-github-app-token`, SHA-pinned) and pass it to **both** release-please steps. Requires creating the App + `RELEASE_BOT_APP_ID` / `RELEASE_BOT_PRIVATE_KEY` secrets (maintainer action — see Open questions). Workflow change lands in this PR; secret creation does not. |
| 2 | P0 | Config forces **3.2.1**, contradicting the binding 1.0.0 decision | `release-please-config.json:3` `bootstrap-sha`, `:11` `"release-as": "3.2.1"`; `.release-please-manifest.json` → `{".": "3.2.0"}`; dry run line 114 `⚠ Setting version for . from release-as configuration`, line 121 `title: chore(master): release 3.2.1`. All ten `x-release-please-version` markers read `3.2.1`. | Set `release-as` to `"1.0.0"` with an inline one-shot note, set the manifest to the value that makes 1.0.0 a forward bump (`"0.0.0"` — see Open questions), reset the ten markers to the same pre-release value, and write the superseding ADR. Verify with `release-pr --dry-run --target-branch <this PR's branch>` and paste the `title: chore(master): release 1.0.0` line into the PR body. |
| 3 | P0 | No gate proves the fragment cut ran before the tag — a published release would ship a CHANGELOG with no version section | `grep -c -i changelog scripts/release/verify-release-version.sh` → 0. `grep -rn rollover-changelog-fragments .github/` → only `rule-enforcement.yml:524`, which runs the script's **unit test**, not the cut. `scripts/release/concat-changelog-fragments.sh --check` → PASS on plain master. `grep -n '^## \[' CHANGELOG.md` → line 6 only (`## [Unreleased]`). Fragments live: added 412, changed 417, removed 10, fixed 688, security 23 (1,550 total) + `_pre_fragment_legacy.md`. `changelog.d/releases/` does not exist. | Extend `verify-release-version.sh` (the tag-time preflight already called from `supply-chain.yml:63`) with fail-closed assertions: exactly one `^## \[$version\] - YYYY-MM-DD$`; `changelog.d/releases/$version.json` exists with `.version == $version`; zero active fragments in the six section dirs; no `_pre_fragment_legacy.md`; `bootstrap-sha` and `release-as` both absent. Add the matching unit-test cases to `scripts/release/tests/test-verify-release-version.sh`. |
| 4 | P0 | `release-publish` environment does not exist; `pypi-publish` has zero protection rules — every write-bearing release job is unprotected | `gh api repos/VMAFx/vmafx/environments` → `github-pages`, `pypi-publish` only. `.../environments/release-publish` → **404**. `.../environments/pypi-publish` → `{"rules": [], "branch": null}`. Twelve jobs declare `environment: release-publish` (`supply-chain.yml:371,481,698`; `docker-publish-production.yml:101,248,343,438,587`; `docker-publish-operator-node.yml:105,207,313`). GitHub auto-creates a referenced environment with no rules. `docs/development/release.md:56` claims both are protected with a required reviewer and `v*` tag policy. | Environment creation is a maintainer action (needs repo admin; out of scope for a code PR). In this PR: add a **live** preflight step in `supply-chain.yml`'s `validate-release` that queries both environments and fails closed unless each carries a `required_reviewers` rule, and correct `release.md:56` to say the setup is required-and-currently-absent. `test-publication-environment-binding.sh` only greps the YAML and cannot see server-side drift. |
| 5 | P1 | `release-as` is a deprecated, **persistent** override applied after commit analysis — left in place it pins every future release to the same version and silently swallows `feat`/`feat!` | Upstream schema marks it `[DEPRECATED] ... Consider using a Release-As commit instead`; upstream manifest docs: "once the release PR is merged you should either remove this or update it ... **Otherwise subsequent runs will continue to use this version**". Dry run line 114 shows it applied unconditionally. ADR-1127 Consequences already require its removal at rollover; that removal has never run. *Caveat*: the auditor's supporting claim of 7 breaking commits since v3.2.0 could **not** be reproduced here — this checkout is shallow (`git rev-parse --is-shallow-repository` → `true`), so `v3.2.0..HEAD` yields only 50 commits, 0 of them `!:`. The finding stands on documented behaviour + the dry-run trace, not on that count. | Keep `release-as: "1.0.0"` for the first cut only, with an inline `_comment` naming it one-shot; `rollover-changelog-fragments.sh:198` already deletes both it and `bootstrap-sha`. Add a hard guard to the Release Script Contract that both keys are absent whenever `.release-please-manifest.json` is ≥ 1.0.0. For any future forced version use a `Release-As: X.Y.Z` commit footer, which is inherently one-shot. |
| 6 | P1 | `Release Script Contract (ADR-1128)` and five sibling rule-enforcement gates are **not required checks** — a red release-script contract does not block merge | Branch protection requires exactly `["Required Checks Aggregator"]`. The aggregator's `required` array (`required-aggregator.yml:53-80`, 28 entries) contains none of: `Release Script Contract (ADR-1128)` (`rule-enforcement.yml:513`), `Deep-Dive Deliverables Checklist (ADR-0108)` (`:27`), `Doc-Substance Gate (ADR-0100 / 0167)` (`:90`), `docs/state.md Touch Gate (ADR-0165)` (`:211`), `FFmpeg-Patches Surface Sync` (`:241`), `ADR Number Collision Guard` (`:315`). `grep -n "Release Script Contract" .github/workflows/required-aggregator.yml` → no match. No admin bypass is needed to merge past them. | Append those six names to the aggregator's `required` array. They trigger on `pull_request` and are draft-gated, so they will genuinely report on every non-draft PR and will not be spuriously absent. Update the required-check inventory in `docs/development/release.md:363-372` in the same PR (ADR-0100). |
| 7 | P1 | release-please **force-recreates** the release branch on every push to master, so the documented hand-added rollover commits are destroyed mid-merge-train | PR #1175's head `d13bab72b` was authored by `github-actions[bot]` and committed 2026-09-02 21:36, two days after the PR was opened (2026-08-31 09:44), with a single commit on the branch. `release-please.yml:10-12` triggers on `push: branches: [master]`. `docs/development/release.md:296-310` instructs the operator to add two commits to that exact branch. | Guard the PR-update step so a hand-finished release PR is never rewritten — extend its `if:` with `&& !contains(steps.draft.outputs.labels, 'autorelease: cut')` and document the label in `release.md`. Operationally the cut is: freeze the merge train → run the two rollover commits → apply `autorelease: cut` → merge. (Alternative, out of scope here: teach `rollover-changelog-fragments.sh` a `--manifest-version-override` so the cut can run in an ordinary pre-release PR against master.) |
| 8 | P1 | The aggregator's absent-means-pass rule green-lights a manifest-only release PR having verified nothing, while the docs claim the opposite | `required-aggregator.yml:170-176`: `if (!run) { core.info('OK (not reported, treated as path-filter-skip)'); continue; }`. A release PR's diff is `.release-please-manifest.json` alone, so all 28 required entries resolve absent. `docs/development/release.md:53-54` claims "The release PR's required CI remains the gate for the broader Netflix golden-data and backend test suites" — false even after finding 1 is fixed. | Add a release-branch `mustReport` list to the aggregator (`Release Script Contract (ADR-1128)`, `Netflix CPU Golden Tests (D24)`, `Build — Ubuntu gcc (CPU) + DNN`) that fails when absent on a `release-please--` head ref, and add `.release-please-manifest.json` to those workflows' path filters so they actually fire. Correct the claim in `release.md`. |
| 9 | P2 | The 1.0.0 switch **downgrades** the advertised pkg-config version, and the SONAME/product-version split is undocumented | `core/meson.build:2` `version : '3.2.1', # x-release-please-version` feeds `pkg_mod.generate(version: meson.project_version())` at `core/src/meson.build:2168`. The ABI SONAME is a *separate*, hardcoded constant: `core/meson.build:19` `vmaf_soname_version = '3.0.0'`, consumed at `core/src/meson.build:2126-2127`. So the maintainer's "libvmaf.so keeps its own 3.x SONAME" is already structurally true — but `libvmaf.pc` would go `3.2.1 → 1.0.0`, which reads as a regression to any consumer pinning on it. | No code change to the SONAME. State the split explicitly in the new ADR and in `docs/development/release.md`: product version (release-please-owned, tag + pkg-config + Python dists + Helm `appVersion` + node image) is independent of `vmaf_soname_version` (`libvmaf.so.3`, hand-bumped only on ABI break). Add an inline comment at `core/meson.build:19` citing the new ADR so nobody "aligns" them later. |
| 10 | P2 | master's ten markers advertise an unreleased `3.2.1` that will never exist on the new number line | Verified all ten carry exactly one marker at `3.2.1`: `core/meson.build:2`, `compat/python-vmaf/__init__.py:7`, `ai/pyproject.toml:3`, `ai/src/vmaf_train/__init__.py:14`, `dev-llm/pyproject.toml:3`, `dev-llm/src/vmaf_dev_llm/__init__.py:18`, `mcp-server/vmaf-mcp/pyproject.toml:3`, `mcp-server/vmaf-mcp/src/vmaf_mcp/__init__.py:15`, `deploy/helm/vmafx/Chart.yaml:10`, `docker/Dockerfile.node:34`. Set by `30a23245f chore(release): prepare single v3.2.1 patch stream (#1174)`. `bash scripts/release/verify-release-version.sh v3.2.1` currently fails at the manifest check. Every container/artifact built from master since 2026-08-31 reports `libvmaf 3.2.1`. | Reset all ten (and the manifest) to the chosen pre-release value in this PR so nothing built from master claims a Netflix-adjacent 3.2.1 that was never released. Record the reset in `docs/state.md`. |
| 11 | P2 | `bootstrap-sha` truncates the first release's generated notes to 31 commits and stays active because zero releases exist | `release-please-config.json:3` → `98dc0b2b...` = `chore(deps): Update dependency onnxruntime (#1168)`, 2026-08-31. Dry run: `⚠ Could not find releases.` / `⚠ Expected 1 releases, only found 0` / `✔ Needed bootstrapping, found configured bootstrapSha 98dc0b2b...` / `✔ Splitting 31 commits by path`. `gh api repos/VMAFx/vmafx/releases` → `0`. Blast radius is bounded by `skip-changelog: true` — only the PR body and the draft-release body are affected, never `CHANGELOG.md`. | Keep it for the first cut (a 1.0.0 whose generated notes enumerate the whole fork history is impractical, and `CHANGELOG.md` is fragment-owned anyway), but state that choice in the ADR and treat the fragment-rendered `## [1.0.0]` section as the authoritative notes. `rollover-changelog-fragments.sh:198` removes it at the cut; finding 5's guard keeps it from returning. |
| 12 | P2 | The generated notes' compare link 404s, and the fork releases into Netflix's tag namespace | Dry-run body: `## [3.2.1](https://github.com/VMAFx/vmafx/compare/v3.2.0...v3.2.1)`. Origin's 27 tags top out at `v3.0.0` / `v3.0.0-rc`; `git ls-remote --tags origin` has no `v3.2.0`. Local `v3.1.0` / `v3.2.0` came from the `upstream` remote and are Netflix's (`3f9e02af2 libvmaf/meson: bump SONAME to 3.2.0`). `git merge-base --is-ancestor v3.2.0 origin/master` → **NO**. All 27 origin tags are inherited Netflix tags; there are zero fork-made tags and zero `*-lusoris.*` tags anywhere. | On the 1.0.0 line the compare base is moot for the first cut (no prior release), and every later cut resolves normally. Close the collision path instead: change the `upstream` fetch refspec to `+refs/tags/*:refs/tags/upstream/*` with `tagOpt = --no-tags`, document it in the `/sync-upstream` skill, and add a guard to `verify-release-version.sh` rejecting a release tag that already exists on Netflix/vmaf. |
| 13 | P2 | `sentence-case` mangles the fork's camelCase identifiers in release notes | `release-please-config.json:50` `"plugins": ["sentence-case"]` (bare string, no exception list). Upstream `SPECIAL_WORDS = ['gRPC', 'npm']` only. Fork subjects that would be destroyed: `cuDNN → CuDNN`, `macOS → MacOS`, `eBPF → EBPF`, `libFuzzer → LibFuzzer`, `isError → IsError`, `arXiv → ArXiv`, `controllerJobID`, `customManagers`. **Does not** conflict with the changelog fragments — `skip-changelog: true` keeps release-please out of `CHANGELOG.md` (ADR-1128), so the two surfaces are disjoint. | Either drop the plugin (`"plugins": []` — it buys only cosmetic capitalisation on a surface the fragment-rendered CHANGELOG supersedes), or switch to the object form with an explicit `specialWords` list. Note the factory does `specialWords ? [...specialWords] : SPECIAL_WORDS`, so a custom list **replaces** the defaults and must re-list `gRPC` and `npm`; also expect an editor schema warning, since `specialWords` is documented only under `linked-versions`. |
| 14 | P2 | Recovery `workflow_dispatch` never restores the `latest` container tags | `type=raw,value=latest,enable=${{ github.event_name == 'release' }}` at `docker-publish-production.yml:139` and `docker-publish-operator-node.yml:139,244,350`. On the documented recovery path (`release.md:97-111`) the event is `workflow_dispatch`, so `latest` is skipped and keeps pointing at whatever the failed run pushed. | Make the guard tag-aware: a preflight step resolves whether `PUBLISH_TAG` equals `releases/latest`, and `enable=` reads that output. Apply to all four sites. |
| 15 | P3 | `release-type: simple` warns on a missing `version.txt` on every run, training operators to ignore the exact signal a renamed marker produces | Dry run: `❯ Fetching version.txt from branch master` / `⚠ file version.txt did not exist` / `version.txt: [class DefaultUpdater]`. `ls version.txt VERSION` → neither exists. Nothing in the repo reads either. **`simple` is nonetheless the right release-type** — `node` would make `package.json` authoritative and `python` demands one canonical version file per package; the ten-file generic fan-out (three Python dists + meson + Helm + Dockerfile) needs `simple` + `generic` extra-files. | Add a one-line `version.txt` so the strategy's canonical file is real, **or** accept the warning and note it in `release.md` as expected output. If added, it must also get an eleventh extra-files entry so `verify-release-version.sh` covers it — otherwise it becomes a new drift surface. |
| 16 | P3 | `pkg/version/version.go` documents a repo-root `VERSION` file that does not exist | `pkg/version/version.go:5-7`: "The value is set to the contents of the repo-root VERSION file at build time via -ldflags". `ls VERSION` → no such file. The real mechanism is `-ldflags "-X .../pkg/version.version=${VMAFX_VERSION}"` with `VMAFX_VERSION` supplied from the publish workflows' `PUBLISH_TAG`. | Rewrite the doc comment to describe the `VMAFX_VERSION` build-arg flow. (Fix-preexisting rule: the file is touched by this PR's version reset anyway if a `version.txt` is added.) |
| 17 | P3 | `docker/Dockerfile.node` is the only annotated Dockerfile; its siblings default to `dev` | `docker/Dockerfile.node:34` `ARG VMAFX_VERSION=3.2.1 # x-release-please-version` vs `docker/Dockerfile.operator:43` and `Dockerfile.go-server:40` → `ARG VMAFX_VERSION=dev`. All three receive the real value from the publish workflows, so the baked default is dead weight that must be maintained and reads as authoritative to a hand-builder. | Prefer aligning `Dockerfile.node` to `=dev` and dropping its `extra-files` entry (`verify-release-version.sh` derives its marker list from that same array, so the two stay consistent automatically). Alternative: annotate all three for symmetry. |
| 18 | P3 | Helm chart `version: 0.1.0` is frozen while `appVersion` tracks releases | `deploy/helm/vmafx/Chart.yaml:9-10`. Intended per ADR-1127 ("Helm chart packaging and Rust crate versions remain independently versioned") and currently harmless: no workflow publishes the chart. Same for the three `Cargo.toml` at `0.1.0` and root `pyproject.toml` at `0.0.0`. | Leave the versions alone; add one-line comments citing the new ADR so nobody "fixes" them later. **Do not** annotate `version:` without first relaxing `verify-release-version.sh` and `rollover-changelog-fragments.sh:113-119`, which both assert exactly one marker per file. |

## Verified-OK

- **The two `release-please-action` invocations are not duplication.** Both live in
  the single `release-please` job, same pinned SHA `45996ed1` (v5.0.0). Step 1
  (`:65-73`) does release-creation only (`skip-github-pull-request: true`); step 2
  (`:75-84`) does PR-creation only (`skip-github-release: true`) and is gated on
  `steps.release.outputs.release_created != 'true'`. The split works around
  release-please running both phases in one process and failing on an invalid
  `previous_tag` when `draft: true` means no tag exists yet (comment at `:61-64`).
- **The draft-detection step is correct** (`release-please.yml:35-59`): `--paginate
  --slurp` with `.[][]` correctly flattens paged output, the SemVer regex correctly
  excludes prereleases, and >1 draft is a hard error requiring operator cleanup
  rather than a silent pick.
- **Nothing can publish without a human.** `supply-chain.yml:9-16` triggers on
  `release: types: [published]` + `workflow_dispatch`, and `"draft": true`
  (config `:17`) means the tag does not exist until an operator clicks Publish.
  **No workflow in the repo has a `push: tags:` trigger** (checked all 30), so a
  stray `git push origin vX.Y.Z` cannot start a publication. This matches the
  maintainer's "never release without asking" rule.
- **`draft: true` is not contradicted by the dry run's `draft: false`.** They are
  different options: the package-level `draft` controls the GitHub *release* draft
  state; the dry-run field reports the *pull request* draft state
  (`draft-pull-request`). A non-draft release PR is correct so its CI runs.
- **`skip-changelog: true` is correct and conflict-free.** release-please never
  touches `CHANGELOG.md`; the file is rendered solely by
  `concat-changelog-fragments.sh` per ADR-1128, and `rule-enforcement.yml:529`
  runs `--check` to gate drift (verified PASS on this tree).
- **All ten `extra-files` carry exactly one marker each** — verified file by file;
  satisfies both `verify-release-version.sh`'s `marker_count -eq 1` and the
  rollover pre-check.
- **`python/setup.py` needs no extra-files entry of its own.** `get_version()`
  parses `__version__` out of `compat/vmaf/__init__.py`; `compat/vmaf` is a symlink
  to `python-vmaf`, resolving to the annotated `compat/python-vmaf/__init__.py:7`.
  It strips a trailing comment, so the marker does not corrupt the parse.
- **Tag shape is consistent across all three consumers.** `include-v-in-tag: true`
  + `include-component-in-tag: false` + `package-name: vmafx` yields `vX.Y.Z`,
  matching the draft-detection regex (`release-please.yml:52`),
  `verify-release-version.sh:25`, and `supply-chain.yml`'s `${RELEASE_TAG#v}`.
- **`versioning: "default"` is right**, and `bump-minor-pre-major` /
  `bump-patch-for-minor-pre-major` are inert rather than wrong — both only take
  effect below 1.0.0. At and above 1.0.0 they are no-ops.
- **The SLSA generator is tag-pinned** (`@v2.1.0`) at `supply-chain.yml:414` and
  `:517`, documented inline and asserted by
  `test-publication-environment-binding.sh` — correct per the project rule, and
  **not** in conflict with the repo's `sha_pinning_required: true`, which exempts
  reusable workflows referenced by `uses:` at job level.
- **Every non-reusable action in the four release-path workflows is SHA-pinned**
  with a version comment.
- **Docker tagging is version-consistent and manifest-bound.** Both publish
  workflows derive every tag from a single `PUBLISH_TAG` and run a
  `validate-release` preflight that rejects prereleases, asserts
  `GITHUB_REF == refs/tags/$PUBLISH_TAG`, runs `verify-release-version.sh`, and
  requires the release to be published/non-draft/non-prerelease.
- **The signing and provenance chain is sound**: `contents: read` at workflow
  level with per-job escalation, `id-token: write` only on cosign/SLSA/PyPI jobs,
  `persist-credentials: false` on every checkout, SBOMs cross-verified against
  per-file SHA-256 before signing, `attach-to-release` refusing to upload unless
  every asset and its `.bundle` is present and non-empty, and PyPI publish
  refusing filename/hash divergence against the immutable index.
- **The absent-means-pass rule is implemented exactly as ADR-0313 describes**
  (`required-aggregator.yml:170-183`), with draft PRs hard-failed, draft-era check
  runs ignored via `minCreatedMs`, a registration grace period, and a poll
  deadline. Its interaction with the release PR is finding 8, not a defect in the
  rule.
- **The dry run performed no repository writes** — no tag, no release, no branch
  push, no PR create/update; terminal action `Would open 1 pull requests`.

### Methodology notes and corrections to the input audits

- **`--config-file` / `--manifest-file` are resolved on the remote branch, not
  locally.** The dry run logs `❯ Fetching release-please-config.json from branch
  master` and fetches every extra-file from master. A local edit therefore
  **cannot** be dry-run tested. The way to produce the maintainer's requested
  1.0.0 evidence is to push the PR branch and re-run with
  `--target-branch <branch>` — the flag exists in the current CLI (verified via
  `release-pr --help`).
- **This checkout is shallow** (`git rev-parse --is-shallow-repository` → `true`),
  so `v3.2.0..HEAD` yields 50 commits here versus the 2,385 an unshallowed clone
  reports. The input audit's "7 breaking commits since v3.2.0" could not be
  reproduced and is **not** used as load-bearing evidence for finding 5. Worth
  unshallowing this tree before any further history-based work.
- **Duplicate findings merged.** Both input audits independently reported
  `release-as` persistence, `bootstrap-sha`, the missing changelog cut gate, the
  missing `v3.2.0` tag, and `version.txt`; these are single findings here (5, 11,
  3, 12, 15).
- **Findings re-severitised against the 1.0.0 decision.** The input audits' "should
  3.2.1 be re-cut as 3.3.0/4.0.0" question is moot — 1.0.0 is binding and is the
  floor regardless of `feat!` content. What survives is that the config still says
  3.2.1 (raised to P0 as finding 2).
- **Findings dropped.** "PR #1175 is missing the rollover / would ship a broken
  3.2.1" is obsolete — #1175 is closed and 3.2.1 will never be cut. Its underlying
  gate gap survives as finding 3.

## Plan — one draft PR, config/workflows/scripts/docs/ADR only

Nothing in this plan tags, publishes, merges, reopens #1175, or triggers
`supply-chain.yml` / `docker-publish-*`. The PR stays **draft** until the
maintainer answers the Open questions.

**Branch**: `chore/release-pipeline-1.0.0-line` in
`/home/kilian/dev/vmaf/.claude/worktrees/tmp-release-audit/` (never in the shared
tree). ADR number reserved with
`scripts/adr/next-free.sh --claim vmafx-first-release-1-0-0` — never hand-picked.

1. **ADR — supersede ADR-1127.** New `docs/adr/NNNN-vmafx-first-release-1-0-0.md`:
   the fork's first release is `v1.0.0` on a fresh number line; every existing tag
   belongs to Netflix upstream history and none is an ancestor of master; the fork
   has never released. `## Context` records that the 3.2.x baseline was a
   *source-version alignment* with Netflix's SONAME, not a fork release.
   `## Decision` states the **product version / ABI split explicitly**: the
   release-please-owned product version (tag, `libvmaf.pc`, three Python dists,
   Helm `appVersion`, node image) is independent of `vmaf_soname_version`
   (`core/meson.build:19`, `libvmaf.so.3`), which is hand-bumped only on ABI break
   and is **not** reset by the 1.0.0 cut.
   `## Alternatives considered` carries the decision matrix (continue 3.2.x /
   `1.0.0` / `4.0.0`). Mark ADR-1127 `Superseded by ADR-NNNN` and add both index
   rows to `docs/adr/README.md`.
2. **Config — retarget to 1.0.0** (finding 2). In `release-please-config.json`,
   set `"release-as": "1.0.0"` with an adjacent `_comment` stating it is one-shot
   and is deleted by `rollover-changelog-fragments.sh` in the release-merge PR;
   keep `bootstrap-sha` for this cut with a `_comment` citing the ADR (finding 11).
   Set `.release-please-manifest.json` to the pre-release value the maintainer
   picks (Q1) and reset all ten `x-release-please-version` markers to the same
   value (finding 10, pending Q2).
3. **Workflow — make release PRs reachable by CI** (finding 1). Add a SHA-pinned
   `actions/create-github-app-token` step to `release-please.yml` and route both
   release-please invocations plus the draft-detection step through
   `steps.app-token.outputs.token`; drop the job's `contents: write` to `read`
   since the App supplies write scope. Guard the PR-update step against
   overwriting a hand-finished cut with `&& !contains(steps.draft.outputs.labels,
   'autorelease: cut')` (finding 7). The App and its two secrets are a maintainer
   action (Q6); until they exist the workflow must **fail loudly** on a missing
   token rather than silently falling back to `GITHUB_TOKEN`.
4. **Gate — prove the cut ran** (finding 3). Extend
   `scripts/release/verify-release-version.sh` with the five fail-closed
   assertions listed in the table, and add the matching cases to
   `scripts/release/tests/test-verify-release-version.sh`. Add the
   `release-as`/`bootstrap-sha`-must-be-absent-at-≥1.0.0 guard (finding 5) to the
   `release-script-contract` job in `rule-enforcement.yml`.
5. **Gate — make the release-critical checks required** (findings 6, 8). Append the
   six `rule-enforcement.yml` job names to `required-aggregator.yml`'s `required`
   array, and add the release-branch `mustReport` list so a `release-please--` head
   ref cannot pass on absence alone. Add `.release-please-manifest.json` to the
   path filters of the three `mustReport` workflows so they actually fire on a
   manifest-only diff.
6. **Gate — assert the publication environments live** (finding 4). Add the
   `gh api .../environments/{release-publish,pypi-publish}` required-reviewer
   preflight to `supply-chain.yml`'s `validate-release`, failing closed.
7. **Hygiene bundle** (findings 13, 14, 16, 17, 18, and 9's comment). Resolve the
   `sentence-case` plugin per Q7; make the four `latest` guards tag-aware; rewrite
   the `pkg/version/version.go` doc comment; align `Dockerfile.node`'s ARG default
   with its siblings and drop its `extra-files` row; add ADR-citing comments at
   `core/meson.build:19`, `deploy/helm/vmafx/Chart.yaml:9`, the three `Cargo.toml`,
   and root `pyproject.toml`. Handle finding 15 per Q8.
8. **Upstream tag-namespace guard** (finding 12). Change the `upstream` fetch
   refspec to `+refs/tags/*:refs/tags/upstream/*` with `tagOpt = --no-tags` in the
   `/sync-upstream` skill and `docs/development/upstream-sync` docs; add the
   "tag must not already exist on Netflix/vmaf" check to
   `verify-release-version.sh`.
9. **Docs (CLAUDE.md §12 r10 — no docs, no merge).** Rewrite
   `docs/development/release.md`: the 1.0.0 first cut, the corrected required-check
   inventory (`:363-372`), the product-version-vs-SONAME split, the
   `autorelease: cut` freeze procedure, the App-token requirement, the
   environments-must-exist prerequisite (replacing the false claim at `:56`), and
   the corrected claim at `:53-54`. Update `docs/rebase-notes.md`,
   `docs/state.md` (a row for the 1.0.0 line and one for master's pre-release
   markers), a `docs/research/` digest on release-please `release-as` /
   `bootstrap-sha` / `sentence-case` semantics, `scripts/release/AGENTS.md`
   invariants, and a `changelog.d/changed/` fragment (ADR-0221).
10. **Verify locally, then verify the version.** `make format-check`, `make lint`,
    `bash scripts/release/tests/*.sh`, `scripts/release/concat-changelog-fragments.sh
    --check`, `bash -n` on every edited workflow, `actionlint`. Then push the draft
    branch and run the **dry run against the branch** to produce the maintainer's
    requested evidence:
    `npx --yes release-please@latest release-pr --repo-url VMAFx/vmafx --token "$(gh auth token)" --dry-run --target-branch chore/release-pipeline-1.0.0-line --config-file release-please-config.json --manifest-file .release-please-manifest.json`
    and paste the `title: chore(master): release 1.0.0` line into the PR body. If it
    does not read 1.0.0, the manifest value from Q1 is wrong — iterate before
    marking ready.

**Deliberately out of scope** (needs repo-admin or a live release; not a code
change): creating the GitHub App and its two secrets, creating the
`release-publish` / `pypi-publish` environments with a required reviewer and `v*`
tag policy, and any tag/release/publish action.

## Open policy questions for the maintainer

1. **Manifest pre-release value.** Set `.release-please-manifest.json` to `"0.0.0"`
   so `0.0.0 → 1.0.0` is a monotone forward bump (recommended — `release-as` then
   agrees with normal computation), or leave `"3.2.0"` and rely on `release-as`
   forcing what is technically a downgrade? The dry run cannot answer this from a
   local edit (config is fetched from the remote branch), so whichever is chosen
   must be proven with `--target-branch` before the PR leaves draft.
2. **Reset master's ten markers now, or leave them at 3.2.1?** Resetting stops
   master-built artifacts from claiming an unreleased, Netflix-adjacent `3.2.1`
   and stops `libvmaf.pc` from later appearing to go backwards — but it also
   changes the pkg-config version on every master build immediately. Leaving them
   means anything built between now and the cut keeps advertising 3.2.1.
3. **`bootstrap-sha` for the 1.0.0 cut.** Keep it at `98dc0b2b` (the first
   release's generated notes cover ~31 commits from 2026-08-31; the authoritative
   notes are the fragment-rendered `## [1.0.0]` section), repoint it further back,
   or remove it so release-please walks the fork's whole history? Removing it makes
   the generated notes enormous but complete.
4. **Erroneous `v4.0.0-lusoris.0` artifacts — delete or keep?** Inspection found
   **no** such git tag (neither on `origin` nor locally) and **zero** GitHub
   releases ever, so there is nothing to delete on those surfaces. GHCR could not
   be checked — this token lacks `read:packages` (403). Should the GHCR
   `ghcr.io/vmafx/*` tags be enumerated and any `4.0.0-lusoris.0` / `latest`
   pointing at it be deleted, and if so by whom?
5. **Netflix tag-namespace collision.** Adopt the `refs/tags/upstream/*` fetch
   refspec plus a "tag must not exist upstream" guard (recommended), or accept the
   shared `vX.Y.Z` namespace and rely on the fork's number line diverging early
   (1.x vs Netflix's 3.x) to keep collisions unlikely for years?
6. **Release-bot identity.** Create a GitHub App (`RELEASE_BOT_APP_ID` +
   `RELEASE_BOT_PRIVATE_KEY`, Contents + Pull requests read/write, installed on
   `VMAFx/vmafx`) — recommended, durable, non-expiring; use a personal PAT, which
   ties the release stream to one identity and expires; or accept that release PRs
   are always admin-merged, which contradicts the "no `--admin` by default" rule
   and defeats findings 6 and 8?
7. **`sentence-case` plugin.** Drop it entirely (`"plugins": []` — simplest, and
   the fragment-rendered CHANGELOG already supersedes the surface it touches), or
   keep it with an explicit `specialWords` list that must re-list `gRPC` and `npm`
   and will raise an editor schema warning?
8. **`version.txt`.** Add a real one (silences the every-run warning, but becomes
   an eleventh coordinated marker that must be covered by
   `verify-release-version.sh`), or leave it absent and document the warning as
   expected output?

## Addendum — 2026-09-03, review round

Two corrections to the plan above, both found while verifying the
implementation against this digest.

**Finding 1 was described but not implemented in the first push.** The prose in
`docs/development/release.md` explained the `GITHUB_TOKEN` suppression, but
`release-please.yml` still carried `token: ${{ secrets.GITHUB_TOKEN }}` at both
release-please steps and `contents: write` on the job. Step 3 of the plan is now
actually applied: a SHA-pinned
`actions/create-github-app-token@bcd2ba49218906704ab6c1aa796996da409d3eb1`
(v3.2.0) step mints the installation token, both release-please invocations and
both read-only `gh api` probes consume `steps.app-token.outputs.token`, the job
permission block drops to `contents: read`, and a preflight step fails the job
with a remediation message when either `RELEASE_BOT_APP_ID` or
`RELEASE_BOT_PRIVATE_KEY` is empty — the plan's "fail loudly rather than fall
back" requirement.

**Step 5 of the plan, taken literally, breaks the release PR it is meant to
unblock.** Promoting all six `rule-enforcement.yml` gates into the aggregator's
`required` array makes two of them permanently red on a machine-generated
release PR, which is the same unmergeable-without-admin-bypass outcome finding 1
exists to remove. Reproduced, not inferred:

- `PR_BODY=<release-please-shaped body> BASE_SHA=… HEAD_SHA=… bash
  scripts/ci/deliverables-check.sh` → exit 1, six
  `::error title=ADR-0108 missing deliverable` lines. release-please writes the
  rendered changelog as the PR body; it contains neither the six-item checklist
  nor any `no <item> needed:` sentinel, and the job's only exemption was a
  `port:` title / `port/` branch.
- The release PR updates `mcp-server/vmaf-mcp/pyproject.toml` (an
  `extra-files` version marker), and the doc-substance gate path-maps
  `^mcp-server/vmaf-mcp/(src/|pyproject\.toml)` to a mandatory `^docs/mcp/`
  edit that a release PR structurally never has → exit 1.

Two more of the six are latent rather than certain: the `docs/state.md` touch
gate trips on a `closes #N` anywhere in the PR body, which a changelog entry can
inherit from a commit subject, and the FFmpeg-patch surface gate is diff-driven
over a public-surface list that a future coordinated marker could intersect.

Resolution: all four *authoring-discipline* gates now call
`scripts/ci/release-pr-exempt.sh` as their first step and skip their work step
when it reports `exempt=true`. The predicate requires a `release-please--` head
ref **and** a bot author, so a human cannot disarm a required gate by branch
name; it is covered by `scripts/ci/tests/test-release-pr-exempt.sh`, which the
deliberately-non-exempt `Release Script Contract` job runs on every PR. The two
correctness gates — Release Script Contract and ADR Number Collision Guard —
stay armed on release PRs. Documented in
`docs/development/release.md`, section "Process gates on the release PR".
