# Release process

The Lusoris fork of libvmaf releases via automation — not manual tag-and-draft.
Pushes to `master` drive a
[release-please](https://github.com/googleapis/release-please-action)
workflow that maintains a release PR, and merging that PR triggers the full
release pipeline (build, sign, publish).

## Version scheme

Releases follow `vX.Y.Z-lusoris.N`:

- `X.Y.Z` tracks the upstream Netflix VMAF version the fork is aligned to.
- `-lusoris.N` is the fork-specific revision. It bumps independently of
  upstream and resets to `.1` when `X.Y.Z` changes.

Example progression:

```text
v3.0.0-lusoris.1  # initial fork release against upstream 3.0.0
v3.0.0-lusoris.2  # second fork revision, still on upstream 3.0.0
v3.1.0-lusoris.1  # reset after upstream 3.1.0 sync
```

Upstream sync PRs (see `/sync-upstream`) update the upstream component; regular
fork PRs advance `-lusoris.N`.

## Automation flow

1. **release-please watches master.** On each push it inspects Conventional
   Commit headers (`feat:`, `fix:`, `docs:`, `chore:`, `ci:`, …) to determine
   whether a release is warranted. If so, it opens or updates a release PR that
   bumps `VERSION`, updates `CHANGELOG.md`, and collects user-visible change
   summaries.
2. **Merging the release PR** tags the commit and triggers the release
   workflow.
3. **The release workflow** builds artefacts (libvmaf binaries, Python wheels),
   runs the Netflix golden-data gate (CPU only — the GPU/SIMD backends
   are covered by per-backend snapshot tests at ULP tolerance, not by
   the goldens), and publishes signed artefacts to GitHub Releases.

## ADR index regeneration policy

`docs/adr/README.md` is the rendered index of every ADR in the fork. Its
"Index" table is generated from per-ADR fragments under
`docs/adr/_index_fragments/<slug>.md` plus an order manifest at
`docs/adr/_index_fragments/_order.txt`. The renderer is
[`scripts/docs/concat-adr-index.sh`](../../scripts/docs/concat-adr-index.sh)
(see [ADR-0221](../adr/0221-changelog-adr-fragment-pattern.md) for why the
pattern exists).

**When adding a new ADR (the common case)** — write the fragment as part of
the same PR and append its slug to `_order.txt`. The PR template's
ADR-index checklist row covers this. Manual append is preferred over
`--write` because it produces a one-line diff that reviewers can verify by
eye and avoids touching unrelated rows.

**When fixing drift between fragments and `README.md` (this sweep's case)**
— run `scripts/docs/concat-adr-index.sh --check` to capture the full diff,
then audit each row against the four drift classes:

- **Silent loss** — fragment exists, README is missing the row.
  Regenerating with `--write` keeps the fragment's row.
- **Orphan content** — README has a row, fragment does not exist.
  Backfill the fragment from the README row (the row content already
  reflects the ADR's accepted state). Do **not** delete the row without
  evidence the ADR is genuinely stale (`Status: Withdrawn` or
  `Superseded` in the ADR file body, plus the underlying decision being
  moot).
- **Reformatted** — same content, different shape (column order, status
  spelling, slug case). Regenerate; the fragment is canonical.
- **Duplicate** — the same row appears more than once in `README.md`,
  usually from a stale append-only edit. Regenerate; the fragment is
  emitted exactly once.

After every fragment-side fix, run `--write` once and verify the README
diff matches the audit's expected shape (rows preserved, duplicates
collapsed, missing rows restored). Reviewers can re-run `--check` against
the rebuilt branch and expect a clean exit.

**Renumbered slugs.** When the dedup sweep referenced in the script's
header comment renumbers an ADR (e.g. `0270-saliency-…` → `0286-saliency-…`),
the fragment must be **renamed** to match the new slug — not duplicated. The
`_order.txt` entry follows the same rename. The fragment body's
`[ADR-NNNN](NNNN-slug.md)` link must match the renumbered slug; mismatches
silently render rows that point at non-existent ADR files. The
fragment-vs-ADR-file slug audit is one line:

```bash
for f in docs/adr/_index_fragments/[0-9]*.md; do
    base=$(basename "$f" .md)
    [[ -f "docs/adr/$base.md" ]] || echo "STALE FRAGMENT: $f"
done
```

## Signing

All release artefacts are signed via
[Sigstore keyless](https://docs.sigstore.dev/cosign/overview/) using the
repository's GitHub OIDC identity. No long-lived signing keys live in the
repo or in CI secrets.

The `supply-chain.yml` workflow uses cosign v3, which emits a single
`.bundle` file per artifact (containing signature + certificate + Rekor
entry). The legacy split `.sig` / `.pem` pair is **not** produced.

### Verifying a binary artifact

```bash
# Replace TAG and FILENAME with the actual release tag and artifact name.
# Available artifacts: libvmaf.so, vmaf (CLI), models.tar.gz,
#   vmaf_mcp-*.whl, vmaf_mcp-*.tar.gz
TAG="v3.x.y-lusoris.N"
FILENAME="vmaf"

gh release download "$TAG" --repo VMAFx/vmafx \
    --pattern "$FILENAME" \
    --pattern "${FILENAME}.bundle"

cosign verify-blob \
    --certificate-identity-regexp "https://github.com/VMAFx/vmafx/.github/workflows/supply-chain.yml@refs/heads/master" \
    --certificate-oidc-issuer "https://token.actions.githubusercontent.com" \
    --bundle "${FILENAME}.bundle" \
    "$FILENAME"
```

### Verifying a container image

```bash
# Replace TAG with the image tag (e.g. v3.x.y-lusoris.N, or with -cuda12 etc.)
TAG="v3.x.y-lusoris.N"
IMAGE="ghcr.io/vmafx/vmafx:${TAG}"

cosign verify \
    --certificate-identity-regexp "https://github.com/VMAFx/vmafx/.github/workflows/docker-publish-production.yml@refs/heads/master" \
    --certificate-oidc-issuer "https://token.actions.githubusercontent.com" \
    "$IMAGE"

# Verify + display the attached CycloneDX SBOM attestation
cosign verify-attestation \
    --certificate-identity-regexp "https://github.com/VMAFx/vmafx/.github/workflows/docker-publish-production.yml@refs/heads/master" \
    --certificate-oidc-issuer "https://token.actions.githubusercontent.com" \
    --type cyclonedx \
    "$IMAGE"
```

### SLSA provenance

SLSA L3 provenance for binary artifacts is attached to each GitHub Release
as `vmaf.intoto.jsonl`. Verify it with
[`slsa-verifier`](https://github.com/slsa-framework/slsa-verifier):

```bash
slsa-verifier verify-artifact vmaf \
    --provenance-path vmaf.intoto.jsonl \
    --source-uri github.com/VMAFx/vmafx \
    --source-tag "$TAG"
```

## CHANGELOG.md fragment workflow (ADR-0221)

The "Unreleased" block of `CHANGELOG.md` is **rendered** from per-PR fragment
files under `changelog.d/<section>/*.md` by
[`scripts/release/concat-changelog-fragments.sh`](../../scripts/release/concat-changelog-fragments.sh).
Sections follow Keep-a-Changelog order: `added` → `changed` → `deprecated` →
`removed` → `fixed` → `security`. The pre-fragment archive lives verbatim in
`changelog.d/_pre_fragment_legacy.md` and is emitted before the section
fragments so existing release-train history is preserved.

### When to add a fragment vs edit `CHANGELOG.md` directly

- **Always add a fragment, never edit `CHANGELOG.md` directly.** Drop a single
  Markdown bullet under `changelog.d/<section>/<topic>.md`. The fragment is
  the source of truth; the rendered `CHANGELOG.md` is a build artefact.
- **Filename convention:** lowercase kebab-case, optionally prefixed with the
  task ID (`T7-39-foo.md`) or ADR number (`adr-0312-deferral-retired.md`)
  for implicit lexical ordering within the section.
- **One fragment per PR.** Multi-surface PRs may ship multiple fragments,
  one per user-discoverable surface, each in the appropriate section.

### When to regenerate (`--write`)

Run `scripts/release/concat-changelog-fragments.sh --write` whenever:

- The `--check` lane fails on CI (drift between fragments and the rendered
  `Unreleased` block).
- A merge has just landed several fragments that are not yet spliced into the
  rendered block.
- A drift-sweep PR is reconciling pre-existing skew (see
  [the 2026-05-08 sweep](#changelog-drift-sweep-historical-context)).

Never edit the rendered "Unreleased" block by hand to add new entries — those
inline edits will be silently overwritten by the next regen.

### Drift classes and resolution policy

Three drift classes can develop between fragments and the rendered block:

| Class | Symptom | Resolution |
| --- | --- | --- |
| **Silent loss** | Fragment exists, no matching row in `CHANGELOG.md`. | Regenerate. The fragment is canonical. |
| **Orphan content** | Row in `CHANGELOG.md`, no matching fragment. | Backfill a fragment if the content is still relevant; delete the row otherwise. Inspect each case manually — never bulk-delete. |
| **Duplicate** | Same entry appears twice (often once from legacy archive, once from a fragment, or once inline + once from a fragment). | Regenerate. The script renders each fragment exactly once. |

`--write` is conservative: it only rewrites the `## [Unreleased]` block.
Released sections below are untouched.

### Drift-sweep cadence

CI runs `--check` on every PR (the docs-fragments lane) so new drift fails
loud. A periodic drift-sweep PR (typically once per merge train) reconciles
the pre-existing skew that accumulates when in-flight PRs add fragments
faster than `--write` is run.

#### CHANGELOG drift sweep — historical context

The 2026-05-08 sweep cleared 13 silent-loss fragments, 1 reformatted entry
(verbose inline → canonical fragment), and 2 duplicate rows
(double `### Changed` header + duplicate FastDVDnet entry). No genuine
orphans were found — every row in `CHANGELOG.md` either had a matching
fragment or lived in the legacy archive.

## Dry-running a release

Before merging a release PR, invoke the `/prep-release` skill locally. It
validates:

- All commits since the last release parse as Conventional Commits.
- The Netflix golden-data gate (CPU scalar + fixed-point) passes.
  GPU / SIMD backends are validated separately via per-backend
  snapshot tests.
- `CHANGELOG.md` renders correctly and references no removed files.
- Signing credentials (OIDC) resolve in the current CI environment.

See the [session orientation](../../CLAUDE.md#11-release) for the one-line
summary and the `/prep-release` skill definition for the full checklist.

## `master` branch protection

`master` is protected at the GitHub API layer — the policy in
[CLAUDE.md §12](../../CLAUDE.md) and [CONTRIBUTING.md](../../CONTRIBUTING.md)
is enforced at the host, not just honored by convention.

- **Required status checks (19):** pre-commit, ruff+mypy+black, semgrep,
  Netflix CPU golden (D24), ASan/UBSan/MSan ×3, Assertion density,
  CodeQL ×4, clang-tidy, cppcheck, Tiny AI, MINGW build, dependency-review,
  gitleaks, shellcheck+shfmt.
- **Linear history required** — merges are squash-or-ff-only.
- **Force-push and deletion disabled.**
- **Admin bypass kept on** (owner can land emergency fixes that skip required
  checks — use sparingly; see the emergency-release section below).
- **Not required (non-blocking signals):** Coverage gate (~40 min — built
  with `-fprofile-update=atomic` since 2026-04-18 to survive parallel-meson
  SIMD-counter races, see [ADR-0110](../adr/0110-coverage-gate-fprofile-update-atomic.md)),
  GPU-advisory jobs, Semgrep OSS.

Management: `gh api --method PUT repos/VMAFx/vmafx/branches/master/protection`
with a JSON payload. The current rule set is documented in
[ADR-0037](../adr/0037-master-branch-protection.md).
When adding or renaming a required CI job, update the `contexts` list.

## Emergency release (out-of-band)

If a CVE requires an out-of-band release that bypasses the release-please PR:

1. Branch off `master` into `hotfix/CVE-YYYY-NNNN`.
2. Land the fix with a `fix:` commit and a signed-off-by line.
3. Manually tag `vX.Y.Z-lusoris.N+1` — release-please will reconcile on the
   next regular push.
4. Backport the CVE fix to any active stacked release branches.

## Upstream parallel

The upstream Netflix release process (manual version bump, manual CHANGELOG
editing, draft-a-release on GitHub) is documented at
[Netflix/vmaf — release.md](https://github.com/Netflix/vmaf/blob/master/resource/doc/release.md).
It does not apply to this fork.
