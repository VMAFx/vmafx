<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-0815: Distroless multi-arch Dockerfiles for vmafx-operator and vmafx-node + release CI

- **Status**: Proposed
- **Date**: 2026-06-03
- **Deciders**: Lusoris
- **Tags**: `ci`, `build`, `security`, `supply-chain`, `github`, `docker`

## Context

The vmafx-operator (Go, CGO_ENABLED=0) and vmafx-node CPU variant need production
container images for Kubernetes deployment. Without a published image, the k8s Operator
(ADR-0711) and Phase 4b platform (ADR-0709) cannot be deployed from the Helm chart
(ADR-0699). The node's CUDA image exists (`docker/Dockerfile.node`, ADR-0717) but is
not wired to any release CI workflow. The operator has no Dockerfile at all.

Using `gcr.io/distroless/static-debian12` as the base image minimises the CVE surface
(no shell, no package manager, no libc beyond musl stubs). Running as the distroless
`nonroot` user (uid 65532, aligned with ADR-0878) satisfies the Kubernetes
`pod-security.kubernetes.io/enforce=restricted` admission profile without extra pod
spec overrides.

Multi-arch (amd64 + arm64) via BuildKit native cross-compilation (CGO_ENABLED=0 makes
this straightforward for pure-Go binaries) avoids QEMU emulation and keeps build times
under two minutes.

## Decision

We will add `docker/Dockerfile.operator` (pure-Go operator binary into distroless,
runs as uid 65532, multi-arch amd64+arm64) and wire it alongside the existing
`docker/Dockerfile.node` (ADR-0717) into a new release CI workflow
(`.github/workflows/docker-publish-operator-node.yml`) that fires on `v*` tags and
`workflow_dispatch`. The workflow builds and pushes `ghcr.io/vmafx/vmafx-operator` and
`ghcr.io/vmafx/vmafx-node` (CPU variant), with cosign keyless signing and syft
CycloneDX SBOM attestation, mirroring the `docker-publish-production.yml` pattern
(ADR-0698).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `gcr.io/distroless/static-debian12` (chosen) | Minimal CVE surface; no shell; aligns with ADR-0878 uid 65532 | No debugging tools in container; must use ephemeral debug containers for prod troubleshooting | Best security posture; debug containers are the k8s-native solution |
| `gcr.io/distroless/cc-debian12` (C runtime variant) | Supports CGO-linked binaries | Larger image; CGO_ENABLED=0 makes this unnecessary for pure-Go operator | Unnecessary for a pure-Go binary |
| `alpine:3.19` | Has shell + apk for ad-hoc debugging; lighter than Debian | Not distroless; larger CVE surface; musl libc can cause subtle differences | CVE surface wider; distroless is the standard for production Go |
| Inline Dockerfile in workflow | Fewer files | Harder to test locally; no `docker build -f` reproducibility | Reproducibility is a first-class requirement |
| Separate Dockerfile per arch | Full control per arch | Doubles maintenance; BuildKit native cross-compile is equivalent | Unnecessary complexity |

## Consequences

- **Positive**: vmafx-operator and vmafx-node (CPU) are now release-published with
  cosign attestation; Helm chart installs work without manual image builds;
  multi-arch amd64+arm64 covers the arm node market without QEMU.
- **Negative**: Two new images add ~2 min to release CI; distroless debugging
  requires ephemeral debug containers (`kubectl debug`).
- **Neutral / follow-ups**: Update `required-aggregator.yml` to include the new
  workflow's smoke-test gate; update `docs/backends/operator.md` runbook with the
  image reference and `docker pull` command.

## Supply-chain impact

- **New dependencies**: none at runtime (pure-Go, static binary).
- **Build-time fetches**: `gcr.io/distroless/static-debian12` base image pulled at
  build time; pinned by digest in the Dockerfile.
- **Sigstore-signable**: cosign keyless signing via Sigstore OIDC is applied to both
  images; syft CycloneDX SBOM is attached as an OCI attestation.
- **CVE surface delta**: narrows — distroless has no shell, no package manager,
  no libc beyond the Go runtime's own calls.

## References

- Open DRAFT PR: #184 (`feat(docker): Dockerfile.operator + node publish CI — distroless multi-arch`).
- ADR-0698: `docker-publish-production.yml` pattern this mirrors.
- ADR-0699: Helm chart that consumes the published images.
- ADR-0709: Phase 4b distributed platform.
- ADR-0711: vmafx-operator implementation.
- ADR-0717: vmafx-node Dockerfile (existing, wired to release CI for the first time).
- ADR-0878: Trivy DS-0002 distroless `nonroot` uid 65532 baseline.
