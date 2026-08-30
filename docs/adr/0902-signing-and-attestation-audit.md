<!-- markdownlint-disable MD013 -->
# ADR-0902: Signing and attestation audit — close residual gaps (2026-05-30)

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris
- **Tags**: security, supply-chain, sigstore, slsa, cosign, attestation, ci, fork-local

## Context

CLAUDE.md §11 advertises "Signing is keyless via Sigstore" for releases. A full
audit (see [`docs/research/signing-and-attestation-audit-2026-05-30.md`](../research/signing-and-attestation-audit-2026-05-30.md))
confirms the bulk of the supply-chain story is already in place:

- Release artefacts (`libvmaf.so`, `vmaf` CLI, `models.tar.gz`, optional
  `u2netp_mirror.{onnx,pth}`): SHA256 → SLSA L3 provenance + `cosign sign-blob`
  bundles + SPDX/CycloneDX SBOM (see
  [`.github/workflows/supply-chain.yml`](../../.github/workflows/supply-chain.yml)).
- `vmaf-mcp` Python package: `cosign sign-blob` + PyPI Trusted Publishing
  with PEP 740 attestations (ADR-0166).
- Production container images (CPU multi-arch, CUDA, ROCm, oneAPI, MCP
  server): `cosign sign` keyless + SBOM via `cosign attest` (some GPU
  variants `continue-on-error: true`, see
  [`.github/workflows/docker-publish-production.yml`](../../.github/workflows/docker-publish-production.yml)).
- OpenSSF Scorecard with Sigstore-signed attestation
  ([`.github/workflows/scorecard.yml`](../../.github/workflows/scorecard.yml)).

The audit surfaced three closeable residual gaps:

1. No GitHub-native `actions/attest-build-provenance` on container images.
   `cosign attest` already publishes the SBOM, but `gh attestation verify` —
   the UX consumers reach for first — relies on the GitHub-native attestation
   format. Adding it costs one job step per image and unlocks the official
   GitHub verification UX without replacing the cosign chain.
2. The post-push `smoke-test` job pulls the freshly-built CPU image by
   digest but never verifies the cosign signature before running it. In a
   compromised-token scenario the signature step could be skipped (or fake
   bundles uploaded) and the smoke test would still pass; verifying the
   signature in-flow closes the trust chain.
3. [`docs/development/release.md`](../development/release.md) advertises
   verification with `cosign verify-blob --certificate-identity-regexp …`
   but never expands the regex or shows a container-image example.
   Consumers wanting to verify a download today have to reverse-engineer
   the OIDC identity from CI logs.

Out of scope (deliberately deferred):

- Git tag signing. release-please writes unsigned annotated tags via the
  GitHub Actions bot identity. Signing tags requires either a GitHub App
  token with GPG attached (no `release-please-action` support today) or a
  post-release-please workflow that re-signs the tag, which would
  rewrite history. The cosign-signed blobs + GitHub-native attestation
  cover artefact-level provenance; tag-level signing adds little on top
  given the linear-history + branch-protection invariants in ADR-0037.
- DCO sign-off enforcement. CLAUDE.md §12 r6 requires Conventional Commits
  only; no DCO policy exists. Adding it would be a policy change, not an
  audit gap.
- Helm chart signing. The chart in [`deploy/helm/vmafx/`](../../deploy/helm/vmafx/)
  has no publishing workflow yet; signing follows once a chart-release
  channel is decided.
- Go binary releases. `cmd/vmafx-*` Go binaries are not yet released as
  standalone artefacts (they ship inside container images, which are
  signed).

## Decision

Close the three closeable gaps in one PR:

1. Add `actions/attest-build-provenance@v3` to every container build job
   in `docker-publish-production.yml` (CPU, CUDA, ROCm, oneAPI, server),
   alongside the existing `cosign sign` step. The attestation runs in
   parallel with the cosign chain — neither replaces the other.
2. Extend the `smoke-test` job to install cosign and verify the CPU image
   signature (`cosign verify --certificate-identity-regexp …
   --certificate-oidc-issuer https://token.actions.githubusercontent.com`)
   before pulling and running it.
3. Expand the "Signing" section of `docs/development/release.md` with a
   copy-pasteable consumer verification recipe for both release blobs
   and container images, including the literal identity regex.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Status quo (cosign-only) | Zero CI delta; already covers signing + SBOM. | `gh attestation verify` UX unavailable; smoke-test runs unverified image; release docs leave verification regex implicit. | Audit deliberately narrows to closeable gaps; status-quo leaves three of them open. |
| Replace cosign with `actions/attest-build-provenance` | Single attestation surface; simpler. | Loses keyless Sigstore SBOM chain (cosign attest), PEP 740 PyPI attestations, and SLSA L3 generator integration. Forces consumers to use `gh attestation verify` exclusively. | Cosign is the established release-channel contract (ADR-0166); replacing it would break existing consumer scripts. Add, don't replace. |
| Sign git tags via post-release-please workflow | Tag-level provenance. | release-please creates the tag in its own job; re-signing requires either rewriting the tag (breaks linear history) or a GitHub App with attached GPG (no first-class support). Adds little on top of artefact + image signatures given ADR-0037 branch-protection. | Deferred; the audit explicitly scoped this out as a non-closeable today gap. |
| Add DCO sign-off enforcement | Standard DCO compliance. | Not a current policy (CLAUDE.md §12 r6 is Conventional Commits only); would be a new rule rather than a gap fix. | Out of scope; would require a separate ADR proposing the policy. |

## Consequences

- **Positive**:
  - `gh attestation verify` works end-to-end on every published container
    image (CPU + 4 GPU/server variants).
  - Smoke test fails closed if a container image lacks a valid Sigstore
    signature, catching CI-token compromise scenarios where signing is
    silently skipped.
  - Consumers have a copy-pasteable verification recipe in
    `docs/development/release.md` — no need to reverse-engineer the OIDC
    identity from CI logs.
- **Negative**:
  - Each container build job grows by ~30 s (cosign verify step on smoke
    test; native attestation runs in parallel with cosign sign and adds
    no critical-path delay).
  - One additional attestation surface for consumers to optionally check;
    the cosign bundle remains canonical for release blobs and PyPI
    artefacts.
- **Neutral / follow-ups**:
  - Tag signing remains an open question; revisit if release-please-action
    grows GPG-via-GitHub-App support.
  - Helm chart signing waits on a chart-publish workflow (separate ADR).
  - Go binary standalone release waits on a `goreleaser` cutover decision
    (separate ADR).

## References

- [`docs/research/signing-and-attestation-audit-2026-05-30.md`](../research/signing-and-attestation-audit-2026-05-30.md) — full audit digest.
- [ADR-0166](0166-mcp-server-release-channel.md) — MCP server release channel (cosign + PyPI Trusted Publishing).
- [ADR-0698](0698-vmafx-production-dockerfile.md) — production container build matrix.
- [ADR-0037](0037-master-branch-protection.md) — branch protection invariants that make tag-level signing low ROI.
- [`.github/workflows/supply-chain.yml`](../../.github/workflows/supply-chain.yml), [`.github/workflows/docker-publish-production.yml`](../../.github/workflows/docker-publish-production.yml), [`.github/workflows/scorecard.yml`](../../.github/workflows/scorecard.yml).
- Source: per user direction — audit the signing setup per CLAUDE.md §11, ship a digest-only PR if everything is well-set, add missing pieces without breaking the existing flow.
