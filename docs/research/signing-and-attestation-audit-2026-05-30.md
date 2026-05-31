# Signing and attestation audit — 2026-05-30

- **Branch**: `chore/signing-and-attestation-audit`
- **Trigger**: audit CLAUDE.md §11 ("Signing is keyless via Sigstore") against
  the actual implementation in `.github/workflows/`.
- **Disposition**: existing setup is genuinely strong; three closeable
  residual gaps. Tracked in [ADR-0902](../adr/0902-signing-and-attestation-audit.md).

## TL;DR

| Surface | Hashes (SLSA input) | Sigstore signature | SBOM | Provenance | Verify recipe |
| --- | --- | --- | --- | --- | --- |
| `libvmaf.so` / `vmaf` CLI / `models.tar.gz` | SHA256 (base64) | `cosign sign-blob` bundle | SPDX + CycloneDX | SLSA L3 (`slsa-github-generator`) | release.md (updated) |
| `u2netp_mirror.{onnx,pth}` (when present) | SHA256 (base64) | `cosign sign-blob` bundle | SPDX + CycloneDX | SLSA L3 | release.md (updated) |
| `vmaf-mcp` wheel + sdist | SHA256 (base64) | `cosign sign-blob` bundle + PyPI PEP 740 attestations | (inherits PyPI metadata) | implicit via PyPI Trusted Publisher | ADR-0166 + release.md |
| Container `vmafx:<tag>` (CPU, amd64+arm64) | image digest | `cosign sign` keyless | `cosign attest` CycloneDX + native `actions/attest-build-provenance` (NEW) | GitHub-native attestation (NEW) | release.md (updated) |
| Container `vmafx:<tag>-cuda12` (amd64) | image digest | `cosign sign` keyless | `cosign attest` (best-effort) + native attestation (NEW) | GitHub-native attestation (NEW) | release.md (updated) |
| Container `vmafx:<tag>-rocm6` (amd64) | image digest | `cosign sign` keyless | — + native attestation (NEW) | GitHub-native attestation (NEW) | release.md (updated) |
| Container `vmafx:<tag>-oneapi2026` (amd64) | image digest | `cosign sign` keyless | — + native attestation (NEW) | GitHub-native attestation (NEW) | release.md (updated) |
| Container `vmafx:<tag>-server` (amd64+arm64) | image digest | `cosign sign` keyless | `cosign attest` (best-effort) + native attestation (NEW) | GitHub-native attestation (NEW) | release.md (updated) |
| Git tag `vX.Y.Z-lusoris.N` | n/a | unsigned annotated tag (release-please default) | n/a | n/a | deferred (see Gaps §G1) |
| OpenSSF Scorecard run | n/a | Sigstore-signed attestation to public dashboard | SARIF uploaded to Security tab | n/a | scorecard public dashboard |

"NEW" = added by this PR.

## Methodology

1. Inventoried `.github/workflows/` for `cosign|sigstore|slsa|attest`.
2. Read every workflow that produces an artefact (release blob, wheel,
   container image).
3. Cross-checked `docs/development/release.md`, ADR-0166 (MCP release
   channel), ADR-0698 (production Dockerfile), CLAUDE.md §11.
4. Checked `.git/hooks/`, `scripts/git-hooks/`, `.pre-commit-config.yaml`,
   `Makefile` for any DCO / sign-off enforcement.

## Workflow inventory

| Workflow | Produces | Signs? | SBOM? | Provenance? |
| --- | --- | --- | --- | --- |
| `supply-chain.yml` | release blobs + MCP wheel/sdist | yes (cosign sign-blob bundles) | yes (SPDX + CycloneDX via anchore/sbom-action) | yes (SLSA L3 via slsa-github-generator) |
| `docker-publish-production.yml` | 5 container images | yes (cosign sign keyless) | yes (cosign attest CycloneDX via syft) | added: `actions/attest-build-provenance` |
| `scorecard.yml` | Scorecard run | yes (Sigstore attestation to public dashboard) | n/a (SARIF) | n/a |
| `docker-image.yml` | (build-only, no push) | n/a | n/a | n/a |
| `release-please.yml` | release PR + git tag | **no** (unsigned annotated tag) | n/a | n/a |
| All others | CI artefacts (logs, coverage, snapshots) | n/a | n/a | n/a |

## Gaps

### G1. Git tag signing — DEFERRED

release-please-action creates unsigned annotated tags via the GitHub Actions
bot identity. Signing tags would require:

- A GitHub App with attached GPG (no first-class `release-please-action`
  support today), OR
- A post-tag workflow that re-signs the existing tag (rewrites tag refs,
  breaks consumers who pin by tag SHA).

The cosign-signed blobs + GitHub-native attestation cover artefact-level
provenance. The branch-protection + linear-history invariants in
[ADR-0037](../adr/0037-master-branch-protection.md) ensure the tagged commit
itself can only have been added via reviewed PR. Tag-level signing adds
little ROI on top.

**Decision**: deferred. Revisit if `googleapis/release-please-action`
ships GPG-via-GitHub-App support.

### G2. No GitHub-native build attestation on container images — FIXED THIS PR

`cosign attest` already publishes the SBOM as an in-toto predicate.
`actions/attest-build-provenance@v3` publishes the **GitHub-native**
attestation that `gh attestation verify` reads. Both formats are useful
to different consumer toolchains; neither replaces the other.

**Action**: add `actions/attest-build-provenance@v4.1.0` to all five build
jobs in `docker-publish-production.yml`. Native attestation runs in
parallel with `cosign sign` so wall-clock impact is negligible.

### G3. Smoke test runs unverified container image — FIXED THIS PR

The `smoke-test` job pulls the freshly-built CPU image by digest and runs
`--version`. The cosign signature step ran in the upstream `build-cpu`
job — but the smoke test never checks it. A compromised CI token could
push an image, skip the signing step, and still pass the smoke test.

**Action**: install cosign in the smoke-test job; run
`cosign verify --certificate-identity-regexp …` against the image digest
before pulling. Failure fails the workflow (signature missing or
mismatched identity).

### G4. Release docs lack a copy-pasteable verification recipe — FIXED THIS PR

`docs/development/release.md` advertises verification with
`cosign verify-blob --certificate-identity-regexp …` but elides the regex
and never shows a container example. Consumers wanting to verify a
download have to reverse-engineer the identity from CI logs.

**Action**: expand the "Signing" section with the literal identity regex,
a release-blob verification example, a container-image verification
example, and a `gh attestation verify` example for the new
GitHub-native attestation.

### G5. DCO sign-off enforcement — OUT OF SCOPE

The brief mentioned "DCO compliance per CLAUDE.md §12 r6". §12 r6 reads:

> Every commit message is Conventional Commits (`type(scope): subject`).
> Enforced by the `commit-msg` git hook.

No DCO sign-off rule exists in CLAUDE.md, AGENTS.md, CONTRIBUTING.md,
`.pre-commit-config.yaml`, or any git hook. Adding DCO would be a new
policy, not an audit gap.

**Decision**: out of scope. Would require a separate ADR proposing the
policy.

### G6. Helm chart signing — DEFERRED

The chart in `deploy/helm/vmafx/` has no publishing workflow. Signing
follows once a chart-release channel is decided (chart-releaser-action +
ghcr.io/OCI chart push, or helm-publish to a GitHub Pages site, etc.).

**Decision**: deferred. Tracked separately.

### G7. Go binary standalone releases — DEFERRED

`cmd/vmafx-*` Go binaries ship inside the signed container images. No
standalone tarball/zip release surface today. If one is added later
(e.g. via `goreleaser`), it should sit alongside the existing
`supply-chain.yml` sign-blob loop.

**Decision**: deferred. Add when standalone Go binary releases land.

### G8. GPU SBOM is best-effort — ACCEPTED AS-IS

The CUDA + server container builds run their SBOM step with
`continue-on-error: true`, meaning syft failures don't fail the
workflow. This is a deliberate trade-off because the GPU base images
sometimes break syft's package inspection (CUDA's package manifest layout
is non-standard).

**Decision**: accepted as-is. The new GitHub-native attestation
(actions/attest-build-provenance) does NOT have this fallback and will
fail closed, so the loss of the cosign-attested SBOM is partly
compensated.

## Consumer verification — quick reference

```bash
# Release blob, verbatim from the bundle layout.
cosign verify-blob --bundle vmaf.bundle vmaf \
  --certificate-identity-regexp 'https://github.com/VMAFx/vmafx/.github/workflows/supply-chain.yml@.*' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com

# Container image, cosign route. Replace DIGEST with the actual sha256 digest.
cosign verify ghcr.io/vmafx/vmafx@sha256:DIGEST \
  --certificate-identity-regexp 'https://github.com/VMAFx/vmafx/.github/workflows/docker-publish-production.yml@.*' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com

# Container image, GitHub-native attestation, NEW.
gh attestation verify oci://ghcr.io/vmafx/vmafx@sha256:DIGEST --repo VMAFx/vmafx
```

The release docs carry the same recipe, kept in sync via the same PR.

## References

- CLAUDE.md §11 — release policy advertising Sigstore.
- ADR-0166 — MCP server release channel.
- ADR-0698 — production Dockerfile.
- ADR-0037 — master branch protection (linear history + no force-push).
- [Sigstore: cosign](https://docs.sigstore.dev/cosign/overview/).
- [SLSA L3 generator](https://github.com/slsa-framework/slsa-github-generator).
- [actions/attest-build-provenance](https://github.com/actions/attest-build-provenance).
- [PEP 740 — index support for digital attestations](https://peps.python.org/pep-0740/).
