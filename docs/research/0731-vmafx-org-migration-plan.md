<!-- markdownlint-disable MD013 MD060 -->
# Research-0731 — VMAFx Org Migration + Monorepo Split Audit

- **Date**: 2026-05-28
- **Author**: lusoris
- **Status**: Accepted (research → execution pending)
- **Parent ADR**: ADR-0686 (VMAFX rebrand umbrella)
- **Tags**: migration, org, split, monorepo, cutover, github

---

## 1. Background

The current primary home is `VMAFx/vmafx` — a private GitHub fork of
`Netflix/vmaf`. The user has created a new GitHub organisation `VMAFx` and
wants to migrate to `VMAFx/vmafx` as a clean (non-fork) repository. The
new repo will carry the full git history and tags from `VMAFx/vmafx` but
will have no GitHub fork relationship with Netflix/vmaf.

Locked decisions captured in this session (2026-05-28):

1. Hard cutover to `VMAFx/vmafx` as the new primary. `VMAFx/vmafx` will be
   archived after the migration.
2. Netflix/vmaf upstream is tracked for backports via `git remote add upstream
   https://github.com/Netflix/vmaf.git` plus operational tooling.
3. `VMAFx/vmafx` was made private immediately; migration will complete before
   re-publishing.
4. Open PRs and issues will be manually transferred (the fork-detach mechanism
   does not carry them automatically).

---

## 2. Repository inventory

Current top-level surface area (as of master at the time of this digest):

| Component | Path | Language | Outbound dependencies | Notes |
|---|---|---|---|---|
| C library + CLI | `libvmaf/` (→ `core/` per PR #1571) | C, Meson | none (standalone) | libvmaf.so, vmaf CLI, vmafx CLI |
| Netflix compat harness | `python/vmaf/`, `compat/python-vmaf/` per #1571 | Python | libvmaf.so via ctypes | Read-only compat layer |
| Netflix golden tests | `python/test/` | Python + YUV fixtures | libvmaf.so | Never modify assertions |
| Tiny-AI training | `ai/` | Python, PyTorch, ONNX | model/, python/vmaf/ | model outputs land in model/ |
| vmaf-tune | `tools/vmaf-tune/` | Python | libvmaf.so, ai/ (optional) | Rate-quality optimizer |
| MCP server | `mcp-server/vmaf-mcp/` | Python | libvmaf.so via subprocess | JSON-RPC server |
| Model registry | `model/` | JSON, pkl, ONNX | consumed by libvmaf + ai/ + mcp-server | Some artifacts via Git LFS |
| FFmpeg patches | `ffmpeg-patches/` | patch files | libvmaf public API | Against n8.1; per-API-change rule |
| Dev container | `dev/` | Containerfile, shell | everything above | vmaf-dev-mcp container |
| Production Docker | `docker/` | Dockerfile | libvmaf.so | Multi-arch; PR #1572 |
| Helm + k8s | `deploy/helm/vmafx/` | YAML, Helm | docker/ images | PR #1570 |
| Docs site | `docs/` | Markdown, mkdocs-material | all surfaces | mkdocs.yml site root |
| Test data | `testdata/` | YUV, JSON | libvmaf (golden snapshots) | GPU/SIMD snapshot JSONs |
| CI workflows | `.github/workflows/` | GitHub Actions | all surfaces | 18 workflow files |
| Release config | `release-please-config.json` | JSON | top-level + ai/ + dev-llm/ + mcp-server/vmaf-mcp/ | 4 packages tracked |
| Scripts | `scripts/` | shell, Python | all surfaces | adr/, ci/, release/ sub-trees |
| Changelog fragments | `changelog.d/` | Markdown | release-please | added/changed/fixed/perf/security |

Total repository size (excluding `.git/`, `build/`): approximately 1,800 tracked files
across ~150,000 lines of code and documentation.

---

## 3. Split option analysis

### Option A — Stay monorepo (`VMAFx/vmafx`)

Everything in a single repository, as today.

**Pros:**

- Cross-component refactors stay in a single PR (e.g., a new C feature extractor
  in `core/` + its Python binding + MCP tool + docs + test all land atomically).
- One release-please cycle, one version number to communicate externally.
- No cross-repo dependency pinning. The AI training code, MCP server, and
  vmaf-tune all reach into the C library's test fixtures and model registry
  without any inter-repo version coordination overhead.
- ADR, state.md, and changelog ownership are unambiguous: one repo.
- The rebrand merge train (PRs #1548–#1573) has already validated that 20+ PRs
  can land cleanly in a single repo. There is no "big repo" operability problem
  at this scale.
- GitHub Actions path-filters (already in use for `docker-image.yml`,
  `ffmpeg-integration.yml`, and others) mitigate the "long CI per unrelated PR"
  problem without a repo split.

**Cons:**

- Model artifacts (ONNX, pkl) can be large; they currently live in `model/` and
  some are in Git LFS. A separate model-release repo isolates binary blob growth
  from code history.
- Clone size grows over time; heavy testdata/ YUV fixtures add to this.
- Downstream users who only want `libvmaf.so` must clone the entire repository
  including ai/, tools/, and docs/.

**Verdict:** Viable and recommended for the near term given single-maintainer
topology. All cons are manageable through existing tooling (path-filters, LFS,
shallow clones via `--filter=blob:none`).

---

### Option B — Aggressive 7-repo split

`VMAFx/vmafx` (C core), `VMAFx/vmafx-tune`, `VMAFx/vmafx-ai`,
`VMAFx/vmafx-mcp`, `VMAFx/vmafx-helm`, `VMAFx/vmafx-dev`, `VMAFx/vmafx-models`.

**Per-split dependency analysis:**

| Repo candidate | Inbound deps | Outbound deps | Release coordination cost |
|---|---|---|---|
| vmafx (C core) | all other repos depend on libvmaf.so | none (standalone) | foundation; must release first |
| vmafx-tune | CI needs libvmaf.so from vmafx | model/ for ONNX models, python/vmaf/ compat | must pin vmafx release SHA; pyproject.toml dep on libvmaf |
| vmafx-ai | K150K corpus, python/vmaf/ compat | model/ outputs consumed by vmafx-tune + vmafx-mcp | bidirectional: ai writes models read by core |
| vmafx-mcp | subprocess calls vmaf binary | model/ for score models | must pin vmafx release |
| vmafx-helm | docker images from vmafx-dev | vmafx release tags | most decoupled; pure deployment |
| vmafx-dev | builds vmafx | vmafx source | circular: dev container needs current source |
| vmafx-models | consumed by vmafx, ai, mcp | none | release-only; high binary blob churn |

**Cross-repo issues identified:**

1. **Circular dev-container dependency.** `dev/Containerfile` builds `libvmaf/`
   from source. In a split, `vmafx-dev` would need to either vendor `vmafx` source
   or pin to a release tag — breaking the current "build from live master" model
   that the CLAUDE.md §12 r15 dev container rule depends on.

2. **Model bidirectionality.** `ai/` writes ONNX outputs into `model/`, which
   `libvmaf/src/dnn/` then loads at runtime. In a multi-repo split, both
   `vmafx-ai` and `vmafx` would need to commit to a stable model-artifact API
   and coordinate releases. At this project's scale this is a pure overhead
   burden.

3. **ADR and state.md fragmentation.** With 7 repos, bug tracking in `docs/state.md`
   and architectural decisions in `docs/adr/` lose their single source of truth.
   The per-PR state.md and ADR rules (CLAUDE.md §12 r8, r13) become unenforceable
   across repo boundaries without additional cross-repo CI tooling.

4. **vmaf-tune test fixtures.** `tools/vmaf-tune/tests/` reference YUV fixtures in
   `python/test/resource/yuv/` and snapshot JSONs in `testdata/`. Cross-repo
   test fixtures require git submodules or LFS-backed artifact servers — both
   of which add friction.

5. **Single-maintainer management overhead.** The Kubernetes-style multi-repo model
   works when each repo has a distinct team. With one maintainer, managing 7
   repositories' CI, secrets, branch protection, release-please manifests, and
   renovate configs multiplies operational surface without adding parallelism.

**Verdict:** Premature optimisation for this project's current scale and maintainer
count. The user's prior assessment ("aggressive 7-way split likely premature
optimization") is confirmed by the dependency analysis.

---

### Option C — Two-repo split (monorepo + models-only)

`VMAFx/vmafx` keeps all code; `VMAFx/vmafx-models` holds `.onnx`/`.pkl`/`.json`
release artifacts as GitHub Releases (not committed to the code repo).

**Analysis:**

This is the industry-standard pattern for ML projects: PyTorch Hub, Hugging Face
model repos, and ONNX Model Zoo all separate code history from binary artifact
history. Key benefits for this project:

- ONNX artifacts can be 50–200 MB each. They currently live in `model/` under Git
  LFS. Moving them to a `VMAFx/vmafx-models` release-only repo eliminates LFS
  churn from the main clone.
- `libvmaf/src/dnn/` already loads models by URL or filesystem path (see
  `dnn/ort_model.c`). Switching from a git-tracked path to a release-artifact URL
  is a one-time loader change.
- `ai/` training scripts could push finished `.onnx` outputs directly to
  `vmafx-models` releases via `gh release upload` rather than committing to `model/`.
- The `model/` directory in the main repo retains only the lightweight `.json`
  descriptor/registry files (< 1 KB each) used for feature-extraction dispatch
  metadata; binary blobs move to the release repo.

**Dependency footprint:**

- `vmafx-models` has zero inbound code dependencies; it is a release artifact sink.
- Consumer code pins artifacts via URL + SHA256 hash (supply-chain-compatible).
- `release-please-config.json` does not need to track `vmafx-models` — binary
  uploads are triggered by the existing `supply-chain.yml` `release:published`
  event.

**Verdict:** This is the right long-term architecture, but it is not a blocker for
the org migration cutover. It can be implemented as a follow-on PR once
`VMAFx/vmafx` is live.

---

## 4. Recommendation

**Option A (strict monorepo) for the cutover day; Option C (monorepo + models repo)
as a follow-on within the next 4 weeks.**

Rationale:

- The cutover is time-sensitive (rebrand PRs are in-flight; the repo is private).
  Introducing a model-artifact split on cutover day adds unnecessary risk.
- The dependency audit confirms that Options A and C are the only operationally
  sound choices for a single-maintainer project. Option B's 7-way split would
  create more CI/release coordination work than the entire current development
  backlog.
- Model-artifact separation (Option C) is a clean, decoupled follow-on that does
  not require touching the C library API, CI workflows, or branch protection
  settings.

**User's priors are validated.** The analysis independently arrives at the same
conclusion: monorepo now, models-only split later.

---

## 5. Cutover execution plan

### 5.1 Pre-cutover gate (complete before cutover day)

The new repo should boot with the rebrand foundation fully landed. The following
PRs are in-flight as of 2026-05-28 and should be merged before or alongside the
cutover:

| PR | Title | Status | Priority |
|---|---|---|---|
| #1548 | SPDX dual-license sweep | OPEN | Must merge — license metadata |
| #1571 | VMAFX repo layout (libvmaf/→core/, python/vmaf/→compat/) | DRAFT | Should merge — structural rename |
| #1565 | VMAFX binary + AI tool aliases | DRAFT | Should merge — user-facing CLI |
| #1568 | C23 bump | DRAFT | Should merge — compiler standard |
| #1564 | Drop legacy build paths | DRAFT | Should merge — cleanup |
| #1567 | CI matrix dedup | DRAFT | Should merge — CI hygiene |
| #1566 | clang-tidy/sanitizer gates | DRAFT | Should merge — quality gate |
| #1570 | Helm chart + k8s | DRAFT | Can merge before or after cutover |
| #1572 | Production Dockerfile | DRAFT | Can merge before or after cutover |
| #1573 | HIP wave32 fix | DRAFT | Can merge before or after cutover |
| #1569 | --netflix-compat flag | DRAFT | Can merge before or after cutover |
| #1562 | dev container entrypoint fixes | DRAFT | Can merge before or after cutover |
| #580 | release-please PR | OPEN | Must NOT auto-merge before cutover; close and re-open in new repo |

**External contributor PRs:** All 42 open PRs in `VMAFx/vmafx` are authored by
`lusoris` (the account running the routines agent). There are no external
contributor PRs that require special handling. The single open issue
(#1371 Dependency Dashboard, from Renovate bot) will be re-created automatically
when Renovate scans `VMAFx/vmafx`.

### 5.2 Cutover day — step-by-step

All steps are reversible until step 6 (archival).

**Step 1 — Create empty repo:**

```text
gh repo create VMAFx/vmafx --public --description \
  "VMAFX — perceptual video quality assessment (GPU + AI extended fork of Netflix/vmaf)" \
  --homepage "https://vmafx.github.io/" \
  --no-clone
```

Do NOT use `gh repo fork` — that would recreate the fork relationship.

**Step 2 — Mirror push (copies all branches, tags, and git history):**

```bash
cd /home/kilian/dev/vmaf   # or any local clone
git push --mirror git@github.com:VMAFx/vmafx.git
```

This copies:

- All branches (master + all in-flight worktree branches)
- All tags (v3.x.y-lusoris.N series)
- All git history

**Step 3 — Set default branch on new repo:**

```text
gh api -X PATCH repos/VMAFx/vmafx -f default_branch=master
```

**Step 4 — Configure branch protection on `VMAFx/vmafx`:**

The current `VMAFx/vmafx` protection is:

- `required_status_checks.strict: false`
- Contexts: `["Required Checks Aggregator"]` (app_id 15368)
- `required_linear_history: true`
- `allow_force_pushes: false`
- `allow_deletions: false`
- `enforce_admins: false`

Apply to new repo:

```bash
gh api -X PUT repos/VMAFx/vmafx/branches/master/protection \
  --input - <<'EOF'
{
  "required_status_checks": {
    "strict": false,
    "contexts": ["Required Checks Aggregator"]
  },
  "enforce_admins": false,
  "required_pull_request_reviews": null,
  "restrictions": null,
  "required_linear_history": true,
  "allow_force_pushes": false,
  "allow_deletions": false
}
EOF
```

Note: the `Required Checks Aggregator` check (app_id 15368) will not fire
until the first PR runs CI in `VMAFx/vmafx`. The protection rule still
blocks merging until CI passes once CI is configured.

**Step 5 — Configure Renovate on `VMAFx/vmafx`:**

Per the session memory, Renovate requires `"forkProcessing": "enabled"` only
on fork repos. Since `VMAFx/vmafx` will not be a fork, Renovate's standard
config should work without that flag. Verify `renovate.json` does not retain
the flag if it was added specifically for the fork scenario.

Also add `VMAFx/vmafx` to the Renovate app installation at
`https://github.com/apps/renovate/installations` (or invite via
`gh api -X PUT repos/VMAFx/vmafx/installation`).

**Step 6 — Re-set CI secrets:**

Secrets in `VMAFx/vmafx` do NOT transfer automatically. Re-create in `VMAFx/vmafx`:

| Secret | Source | Notes |
|---|---|---|
| `GITHUB_TOKEN` | Auto-provisioned by GitHub Actions | No action needed |
| Sigstore / OIDC signing | OIDC via `id-token: write` in supply-chain.yml | Keyless; no secret to migrate — the OIDC issuer in the attestation will change from `VMAFx/vmafx` to `VMAFx/vmafx` |
| OpenSSF Scorecard token | `SCORECARD_TOKEN` or GitHub OIDC | Re-register scorecard badge URL to `VMAFx/vmafx` |

This project's supply-chain workflow uses keyless Sigstore signing (OIDC, not a
stored COSIGN_KEY). There are no long-lived signing secrets to migrate. The SLSA
provenance for future releases will reference `VMAFx/vmafx` automatically once
the workflow runs in the new repo.

**Step 7 — Update redirect and archive `VMAFx/vmafx`:**

```bash
# Update VMAFx/vmafx README (add migration notice at top)
# Then archive
gh repo archive VMAFx/vmafx
```

### 5.3 Post-cutover file updates

**Files requiring `VMAFx/vmafx` → `VMAFx/vmafx` substitution:**

| File | References to update |
|---|---|
| `mkdocs.yml` | `site_url`, `repo_url`, `repo_name`, social link |
| `README.md` | 6 badge URLs, ko-fi link text |
| `release-please-config.json` | `"package-name": "vmaf-lusoris"` → `"vmafx"` |
| `docs/adr/0011-versioning-lusoris-suffix.md` | repo URL cross-refs |
| `docs/adr/0002-merge-path-master-default.md` | repo URL cross-refs |
| `docs/adr/0008-readme-fork-rebrand.md` | repo URL cross-refs |
| ~329 other `docs/` files | grep pass: `grep -rl 'VMAFx/vmafx' docs/` |
| `docs/benchmarks.md` | badge/link URLs |
| `docs/index.md` | home page URLs |
| `.github/workflows/scorecard.yml` | if it references the repo URL directly |
| `.github/workflows/release-please.yml` | tag naming: `v3.x.y-lusoris.N` → `v3.x.y-vmafx.N` (coordinate with ADR-0011 if this changes) |

**GHCR image names:** The `docker-image.yml` workflow builds but does not push
to GHCR today (it has `continue-on-error: true` advisory status). Once
`VMAFx/vmafx` is live and the production Dockerfile PR (#1572) lands, the GHCR
image namespace will be `ghcr.io/vmafx/vmafx` (from the `${{ github.repository }}`
variable in the workflow). No manual image rename is needed — the first push from
the new repo will create the new namespace.

**Git remotes in local clones and worktrees:**

```bash
# In the primary clone
git remote set-url origin git@github.com:VMAFx/vmafx.git
# Upstream tracking stays the same
git remote set-url upstream https://github.com/Netflix/vmaf.git
```

Active worktrees (`git worktree list`) each maintain separate `.git` file pointers.
After updating `origin` in the primary clone, all worktrees in
`/home/kilian/dev/vmaf/.claude/worktrees/` inherit the updated remote URL
automatically (they share the same `.git/` directory).

**Dev container config:** `dev/Containerfile` and `dev/docker-compose.yml` do not
hardcode the GitHub URL; they build from the local workspace mount. No update
needed there. However, `fix/hardcoded-repo-paths-sweep-20260527` (PR #1554) should
land before cutover to remove the last `/home/kilian/dev/vmaf` hardcodes in Python
scripts.

### 5.4 Upstream Netflix/vmaf tracking

After cutover, Netflix/vmaf upstream tracking works as follows:

**Local clone:**

```bash
git remote add upstream https://github.com/Netflix/vmaf.git
# Already present in current clone; will transfer via mirror-push to new repo
# remote config (git remote entries are not part of the mirror push — must be
# re-added in each new clone of VMAFx/vmafx)
```

**Operational tracking workflow (skeleton for `.github/workflows/upstream-watcher.yml`):**

The existing `upstream-watcher.yml` workflow already implements this pattern.
After cutover, verify the workflow's `GITHUB_TOKEN` permissions cover
`contents: read` for reading upstream and `issues: write` if it opens issues.
The workflow should reference `Netflix/vmaf` as the upstream remote — confirm
the `scripts/upstream-watcher/` scripts still point to the correct remote name.

No changes to the upstream-tracking approach are needed; it is repo-location
agnostic.

### 5.5 Open PR and issue transfer plan

**Open PRs (42 total, all authored by lusoris):**

The 42 open PRs in `VMAFx/vmafx` fall into three categories:

| Category | Count | PRs | Recommendation |
|---|---|---|---|
| Must merge before cutover (foundation) | ~8 | #1548, #1571, #1565, #1568, #1564, #1567, #1566, #580 (close) | Land or close before cutover |
| In-flight feature work (can continue in new repo) | ~26 | #1544, #1549–#1563, others | Re-push branches to `VMAFx/vmafx`; reopen PRs there (mirror-push preserves branches) |
| Renovate dependency bumps | 4 | #1563, #1543, #1542, #1540 | Close; Renovate will re-open in new repo |
| Archived Routines-authored research | ~4 | #1550–#1557 | Close after merging any landed research docs |

Because `git push --mirror` copies all branches to `VMAFx/vmafx`, the diff for
each in-flight PR is already present in the new repo. Reopening them requires
only a `gh pr create` in the new repo context pointing at the already-pushed
branch. The PR body and title can be carried over from `VMAFx/vmafx` via
`gh pr view <N> --json body,title`.

**Open issues (1 total):**

Only the Renovate Dependency Dashboard (#1371) is open. It will be auto-recreated
by Renovate within 24 hours of it scanning `VMAFx/vmafx`.

### 5.6 Rollback plan

| Stage | How to roll back |
|---|---|
| Before step 6 (archive) | Delete `VMAFx/vmafx` (no archive was created yet), re-publicise `VMAFx/vmafx`, update remote URLs back |
| After archiving `VMAFx/vmafx` | `gh repo unarchive VMAFx/vmafx` (archival is reversible); delete `VMAFx/vmafx` if needed |
| After deleting `VMAFx/vmafx` | Not applicable — user chose to archive, not delete |

**What gets stuck after archival:** GitHub Actions in `VMAFx/vmafx` will stop
running. PRs and issues in `VMAFx/vmafx` become read-only. Any external links
to `VMAFx/vmafx` (docs badges, issue URLs, external blog posts) will 404 or
redirect to the archived repo page — not to `VMAFx/vmafx`. GitHub does NOT
provide automatic redirect for archived repos that were not transferred via
"Transfer repository."

**Mitigation:** The README redirect added in step 7 is the only redirect mechanism
available without a true GitHub repository transfer. If the user later wants a
proper redirect (HTTP 301 from `VMAFx/vmafx` → `VMAFx/vmafx`), GitHub's
"Transfer repository" feature (Settings → Transfer) is still available on an
archived repo and would set up a 1-year HTTP redirect. The hard cutover (mirror
push + archive) is chosen for the fork-relationship removal benefit; the tradeoff
is the absence of automatic redirect.

---

## 6. Models-only split follow-on (Option C sketch)

Defer to a post-cutover PR. The implementation would be:

1. Create `VMAFx/vmafx-models` as a release-only repo with no tracked code
   — only GitHub Releases with attached `.onnx`, `.pkl`, `.json` artifacts.
2. Update `libvmaf/src/dnn/ort_model.c` and the model loader to accept HTTP
   URLs in addition to filesystem paths (or use a sidecar download script).
3. Remove binary blobs from `model/` in `VMAFx/vmafx`; retain only the
   lightweight JSON descriptor files.
4. Update `ai/` training scripts to push finished ONNX exports to
   `vmafx-models` via `gh release upload` rather than committing to `model/`.
5. Update `supply-chain.yml` to sign model artifacts in the `vmafx-models`
   release, not the main release.

This change is entirely decoupled from the org migration and can land in a
separate PR at any time after cutover.

---

## 7. Checklist summary for cutover day

- [ ] Merge #1548 (SPDX sweep)
- [ ] Merge or close #580 (release-please PR — its base is `VMAFx/vmafx` master)
- [ ] Land PR #1554 (hardcoded path sweep) to clean repo before mirror
- [ ] Create `VMAFx/vmafx` (empty, public, no fork relationship)
- [ ] `git push --mirror git@github.com:VMAFx/vmafx.git`
- [ ] Set default branch to `master` on new repo
- [ ] Apply branch protection (linear history, no force-push, Required Checks Aggregator)
- [ ] Update `mkdocs.yml`, `README.md`, `release-please-config.json` in a follow-up PR
- [ ] Re-point dev clone remotes to `VMAFx/vmafx`
- [ ] Add `VMAFx/vmafx` to Renovate app installation
- [ ] Add redirect note to `VMAFx/vmafx` README
- [ ] Archive `VMAFx/vmafx`
- [ ] Verify first CI run passes in `VMAFx/vmafx`
- [ ] Open follow-on URL sweep PR for the 332 doc files

---

## 8. Go/no-go verdict

**GO.** The dependency analysis confirms there are no structural blockers. The
mirror-push mechanism preserves 100% of git history and tags. The only risks are
the absence of an automatic GitHub redirect and the manual PR re-opening effort —
both are acceptable given the project's single-maintainer topology and the fact
that all 42 open PRs are owned by the same GitHub account.

**Primary prerequisite before execution:** Merge the SPDX license sweep (#1548)
and close the release-please PR (#580) so the new repo's first CI run starts from
a clean foundation.

---

## References

- ADR-0686 VMAFX rebrand umbrella
- User decision session 2026-05-28 (Q1–Q4 popup answers: hard cutover, track
  upstream via git remote + workflow, repo now private, manual PR transfer)
- `gh api repos/VMAFx/vmafx/branches/master/protection` — branch protection
  export used in §5.2 step 4
- Open PR inventory from `gh pr list --repo VMAFx/vmafx --state open --limit 50`
- Research-0730 Intel Arc cross-backend parity (precedent for research numbering)
