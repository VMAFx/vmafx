<!-- markdownlint-disable MD051 -->
# Release process

VMAFx releases through release-please and an explicit publication gate.
Pushes to `master` drive a
[release-please](https://github.com/googleapis/release-please-action)
workflow that maintains a release PR. Merging that PR creates a draft release;
publishing the draft creates the tag and triggers the full build, signing, and
publication pipeline.

## Version scheme

Releases follow ordinary SemVer tags, `vX.Y.Z`:

- `X` changes for incompatible public-surface changes.
- `Y` changes for backward-compatible features.
- `Z` changes for backward-compatible fixes.

Example progression:

```text
v3.2.1  # patch release
v3.3.0  # backward-compatible feature release
v4.0.0  # incompatible public-surface release
```

The VMAFx release stream advances independently of Netflix/vmaf. Upstream
alignment remains recorded in sync commits and release notes, not encoded in
the tag. [ADR-1127](../adr/1127-single-semver-release-stream.md) governs this
policy.

## Automation flow

1. **release-please watches master.** On each push it inspects Conventional
   Commit headers (`feat:`, `fix:`, `docs:`, `chore:`, `ci:`, …) to determine
   whether a release is warranted. If so, it opens or updates one release PR
   that bumps the root manifest and every coordinated version marker.
2. **Finalize the generated release PR.** After all other release changes are
   merged, regenerate Unreleased and run the fragment rollover with the PR's
   exact version and UTC date. Commit that result as the release PR's final
   change. Any later fragment invalidates the cut and must be rolled again.
3. **Merging the release PR** creates a draft GitHub release. It does not yet
   create the public release tag.
4. **An authenticated operator publishes the draft.** Publication creates the
   `vX.Y.Z` tag and emits the `release.published` event. This explicit gate is
   required because GitHub suppresses most follow-on workflow events created
   by the repository `GITHUB_TOKEN`.
5. **The publication workflows** check out the published release's immutable
   tag rather than the default branch. The Docker workflows derive their image
   tags from the same `github.event.release.tag_name`; the other release jobs
   build libvmaf binaries and Python wheels. Every workflow signs and attests
   its outputs, runs its own smoke checks, and publishes only after those gates
   pass. The release PR's required CI remains the gate for the broader Netflix
   golden-data and backend test suites.

### Native Linux release layout

The native files attached by `supply-chain.yml` are currently Linux ELF
artefacts. Meson builds a three-name dynamic-library chain: `libvmaf.so`, its
ABI SONAME such as `libvmaf.so.3`, and its ABI real name such as
`libvmaf.so.3.0.0`. GitHub artifact and release downloads do not preserve
symlinks, so the workflow publishes all three names as identical regular-file
assets. Each name is hashed, inventoried in both native SBOMs, signed, and
covered by the native SLSA provenance.

Download the CLI with the entire library chain, restore the raw CLI asset's
executable bit, and keep the files together when running it:

```bash
mkdir vmafx-linux && cd vmafx-linux
gh release download v3.2.1 --repo VMAFx/vmafx \
  --pattern vmaf --pattern 'libvmaf.so*'
chmod +x vmaf
LD_LIBRARY_PATH="$PWD" ./vmaf --version
```

The clean-environment release gate exercises that same layout after an
artifact upload/download round trip and requires the CLI's `DT_NEEDED` entry
to resolve to the staged SONAME file. Windows `vmaf.exe` remains a CI build
artifact, not a GitHub Release asset, and this workflow currently publishes no
macOS native CLI or dylib. Use the production containers or build from source
for those platforms until platform-specific release bundles are introduced.

### Supply-chain recovery dispatch

Use the manual supply-chain dispatch only to recover an existing, published,
non-prerelease GitHub release. Run the workflow at the immutable tag ref and
pass that same tag as its input:

```bash
tag=vX.Y.Z; gh workflow run supply-chain.yml --ref "$tag" -f tag="$tag"
```

The tag/ref equality is a release invariant: dispatching from `master` or from
a different ref would make rebuilt artefacts and their signed provenance refer
to source other than the published release.

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

### What is signed

- **Release blobs** (`libvmaf.so*`, `vmaf`, `models.tar.gz`, optional
  `u2netp_mirror.{onnx,pth}`): cosign sign-blob bundles attached to the
  GitHub Release. SLSA L3 provenance via
  `slsa-framework/slsa-github-generator`. SPDX + CycloneDX SBOM attached.
- **`vmaf-mcp` Python package** (wheel + sdist): cosign sign-blob bundles
  plus PEP 740 attestations stored alongside the PyPI artefact
  (Trusted Publishing, no token). See
  [ADR-0166](../adr/0166-mcp-server-release-channel.md).
- **Production container images** (`ghcr.io/vmafx/vmafx:<tag>` and the
  `-cuda13` / `-rocm7` / `-oneapi2025` / `-server` variants): cosign keyless
  signature plus a GitHub-native build-provenance attestation
  (`actions/attest-build-provenance`). See
  [ADR-0902](../adr/0902-signing-and-attestation-audit.md).
- **Go service images** (`ghcr.io/vmafx/vmafx-server:<tag>`,
  `ghcr.io/vmafx/vmafx-operator:<tag>`, and
  `ghcr.io/vmafx/vmafx-node:<tag>`): the same cosign signature, CycloneDX SBOM,
  and GitHub-native build provenance, emitted by
  `docker-publish-operator-node.yml`.

### Consumer verification recipes

The OIDC identity is the workflow path inside the repo. The release blobs
and MCP wheel come from `supply-chain.yml`; the container images come from
`docker-publish-production.yml`.

```bash
# Release blob. Every vmaf/libvmaf.so* asset has a matching FILE.bundle.
cosign verify-blob --bundle vmaf.bundle vmaf \
  --certificate-identity-regexp 'https://github.com/VMAFx/vmafx/.github/workflows/supply-chain.yml@.*' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com

# vmaf-mcp wheel on PyPI. The PyPI integrity API supplies the PEP 740
# provenance; pypi-attestations binds it to the expected source repository.
pypi-attestations verify pypi \
  --repository https://github.com/VMAFx/vmafx \
  pypi:vmaf_mcp-3.x.y-py3-none-any.whl

# Container image, cosign route. Replace DIGEST with the actual sha256 digest.
cosign verify ghcr.io/vmafx/vmafx@sha256:DIGEST \
  --certificate-identity-regexp 'https://github.com/VMAFx/vmafx/.github/workflows/docker-publish-production.yml@.*' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com

# Container image, GitHub-native attestation route (added by ADR-0902).
gh attestation verify oci://ghcr.io/vmafx/vmafx@sha256:DIGEST --repo VMAFx/vmafx

# Go server/operator/node images use their own workflow identity.
cosign verify ghcr.io/vmafx/vmafx-node@sha256:DIGEST \
  --certificate-identity-regexp 'https://github.com/VMAFx/vmafx/.github/workflows/docker-publish-operator-node.yml@.*' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com
```

The post-push smoke jobs in both Docker workflows run the matching cosign
verification recipe before pulling an image. The production workflow also
executes the CPU CLI and the Python 3.14 server entrypoints; the Go-service
workflow starts the Go scoring server and probes `/healthz` plus `/readyz`,
checks the operator version, and executes `vmaf --version` plus `ffmpeg -version`
from the node image. A signature or runtime-linkage gap fails the release rather
than silently shipping a broken image.

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

### Cutting a release from fragments

Release-please has `skip-changelog: true`; it never edits `CHANGELOG.md`.
Once the generated release PR contains the final manifest and version-marker
updates, run:

```bash
scripts/release/concat-changelog-fragments.sh --write
git commit -am 'docs(release): render final 3.2.1 notes'
scripts/release/rollover-changelog-fragments.sh \
  --version 3.2.1 --date 2026-08-31
git add CHANGELOG.md changelog.d
git commit -m 'chore(release): cut 3.2.1 changelog'
```

Replace the example version and UTC date for later releases. The rollover
requires a clean tree, exact agreement between the root manifest and every
coordinated marker, zero renderer drift, a unique target heading, and a
non-empty active source set. It then removes the consumed fragments and legacy
source, leaving their exact content in the versioned changelog section and a
SHA-256 receipt under `changelog.d/releases/`. The removals are recoverable
from Git history. A second identical invocation is a no-op.

[ADR-1128](../adr/1128-fragment-owned-release-cuts.md) governs this cutover.

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

Run the release-please preview from an origin-faithful clone, not directly from
the development checkout. Local tags include tags fetched from the Netflix
`upstream` remote; those tags do not necessarily exist in `VMAFx/vmafx` and can
make a local preview select the wrong previous release instead of the configured
bootstrap SHA. The preview clone must expose only the fork's advertised tags
and the candidate `master` tree. Supply credentials through a protected file
descriptor or token-file path so the CLI never echoes a literal token in its
argument list.

See the [session orientation](../../CLAUDE.md#11-release) for the one-line
summary and the `/prep-release` skill definition for the full checklist.

## `master` branch protection

`master` is protected at the GitHub API layer — the policy in
[CLAUDE.md §12](../../CLAUDE.md) and [CONTRIBUTING.md](../../CONTRIBUTING.md)
is enforced at the host, not just honored by convention.

- **Required status checks (25):** Build — Ubuntu gcc (CPU) + DNN,
  Build — Ubuntu clang (CPU) + DNN, Build — Windows MinGW64 (CPU),
  Build — Windows MSVC + CUDA, Build — Windows MSVC + oneAPI SYCL,
  Build — Ubuntu HIP (T7-10b runtime), CodeQL ×4, Pre-Commit (Formatters),
  Python Lint (Ruff + Black + isort + mypy), Semgrep, Clang-Tidy, Cppcheck,
  Dependency Review, Gitleaks, Docs, ShellCheck + shfmt, Netflix CPU golden (D24),
  ASan/UBSan/MSan ×3, Assertion Density, Tiny AI (DNN Suite + ai/ Pytests).
  Enforced via the Required Checks Aggregator
  (`.github/workflows/required-aggregator.yml`).
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
3. Manually tag the next ordinary patch version, `vX.Y.(Z+1)`;
   release-please will reconcile on the next regular push.
4. Backport the CVE fix to any active stacked release branches.

## Upstream parallel

The upstream Netflix release process (manual version bump, manual CHANGELOG
editing, draft-a-release on GitHub) is documented at
[Netflix/vmaf — release.md](https://github.com/Netflix/vmaf/blob/master/resource/doc/release.md).
It does not apply to this fork.
