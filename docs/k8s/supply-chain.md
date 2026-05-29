# VMAFX Container Supply-Chain Verification

This document describes the supply-chain hardening applied to every VMAFX
production container image and explains how to verify an image before deploying
it. See [ADR-0798](../adr/0798-supply-chain-hardening.md) for the decision
record.

## Images published

All images are pushed to the GitHub Container Registry (GHCR) at
`ghcr.io/vmafx/vmafx` or `ghcr.io/vmafx/vmafx-controller`.

| Image tag suffix | Base (runtime) | Architectures | Distroless |
|------------------|----------------|---------------|------------|
| *(none / latest)* | `gcr.io/distroless/cc-debian12` | amd64 + arm64 | Yes |
| `-server` | `gcr.io/distroless/cc-debian12` | amd64 + arm64 | Yes |
| `-cuda12` | `gcr.io/distroless/cc-debian12` | amd64 | Yes |
| `-rocm6` | `gcr.io/distroless/cc-debian12` | amd64 | Yes |
| `-oneapi2026` | `gcr.io/distroless/cc-debian12` | amd64 | Yes |
| `-vulkan` | `gcr.io/distroless/cc-debian12` | amd64 + arm64 | Yes |
| `vmafx-controller` | `gcr.io/distroless/cc-debian12` | amd64 + arm64 | Yes |

All runtime images use `gcr.io/distroless/cc-debian12` — a minimal glibc +
libssl image with no shell, no package manager, and no unnecessary userspace
binaries. The build stages run on `debian:12-slim` or `debian:13-slim`
(the latter providing newer compiler toolchains), but nothing from the build
stage carries into the runtime layer except the compiled binaries and
model files.

## What is applied per image

1. **Distroless base** — attack surface is reduced to glibc + libssl only.
2. **Multi-arch build matrix** — CPU and server images are built for
   `linux/amd64` and `linux/arm64` via QEMU cross-compilation. GPU variants
   are `linux/amd64` only where the GPU SDK is not available for arm64.
3. **Cosign keyless signing** — every image digest is signed via
   [Sigstore](https://www.sigstore.dev/) OIDC after push. The signing
   certificate is bound to the GitHub Actions OIDC identity, not a long-lived
   key material.
4. **SBOM via syft** — a CycloneDX JSON bill of materials is generated for
   every image and attached as a cosign attestation.
5. **SLSA Build Level 3 provenance** — build provenance covering all release
   binaries (`libvmaf.so`, `vmaf` CLI, models, `vmaf-mcp` wheel) is generated
   by the `slsa-github-generator` reusable workflow and uploaded to the GitHub
   Release. Container-level `actions/attest-build-provenance` attestations are
   attached per digest.
6. **Trivy scan** — the CPU image is scanned for OS and library CVEs after
   push. The build gate fails on any `HIGH` or `CRITICAL` unpatched finding.
   SARIF results are uploaded to the repository Security tab.

## Verifying an image

Install [cosign](https://docs.sigstore.dev/cosign/installation/) and
[syft](https://github.com/anchore/syft) before running these commands.

### Verify the Sigstore signature

```bash
IMAGE="ghcr.io/vmafx/vmafx:v3.x.y-lusoris.N"

cosign verify \
  --certificate-identity-regexp "https://github.com/lusoris/vmaf/.github/workflows/docker-publish-production.yml" \
  --certificate-oidc-issuer "https://token.actions.githubusercontent.com" \
  "${IMAGE}"
```

A successful verification prints the signing certificate and Rekor entry. Any
tampered or unsigned image exits non-zero.

### Inspect the attached SBOM

```bash
# Retrieve the CycloneDX attestation attached at build time
cosign verify-attestation \
  --type cyclonedx \
  --certificate-identity-regexp "https://github.com/lusoris/vmaf/.github/workflows/docker-publish-production.yml" \
  --certificate-oidc-issuer "https://token.actions.githubusercontent.com" \
  "${IMAGE}" \
  | jq -r '.payload' | base64 -d | jq '.predicate'
```

Alternatively, use syft to regenerate an SBOM from the pulled image and
compare it against the attested one:

```bash
syft "${IMAGE}" --output cyclonedx-json | jq '.components[].name' | sort
```

### Verify SLSA provenance (release binaries)

Download the `.intoto.jsonl` provenance file from the GitHub Release assets,
then verify it with the SLSA verifier:

```bash
slsa-verifier verify-artifact \
  --provenance-path vmaf-v3.x.y-lusoris.N.intoto.jsonl \
  --source-uri github.com/lusoris/vmaf \
  --source-tag v3.x.y-lusoris.N \
  libvmaf.so
```

### Check Trivy scan results

Trivy results are uploaded to the repository Security tab after each release
build. You can also run a local scan against any pulled image:

```bash
trivy image \
  --severity HIGH,CRITICAL \
  --exit-code 1 \
  "${IMAGE}"
```

## Builder image pinning

All builder base images are pinned by SHA-256 digest in the Dockerfiles. This
prevents silent base-image drift between builds. The pins are reviewed and
updated via Renovate Bot PRs.

| Dockerfile | Builder base | Runtime base |
|------------|-------------|--------------|
| `docker/Dockerfile.production` | `debian:13-slim@sha256:b6e2a…` | `gcr.io/distroless/cc-debian12@sha256:aa0b7…` |
| `docker/Dockerfile.production-gpu` | `debian:13-slim@sha256:b6e2a…` | `gcr.io/distroless/cc-debian12@sha256:aa0b7…` |
| `docker/Dockerfile.controller` | `golang:1.23-bookworm` + `debian:12-slim@sha256:0104b…` | `gcr.io/distroless/cc-debian12@sha256:aa0b7…` |

Note: `Dockerfile.production` and `Dockerfile.production-gpu` use
`debian:13-slim` (Trixie) for the build stage because it carries newer
compiler toolchains needed for AVX-512 and C++23 features, while the runtime
layer remains `cc-debian12` (Bookworm/glibc 2.36). Compiled binaries are
statically linked where possible; the only dynamic dependency is glibc itself,
which is present in the distroless base.

## Related documents

- [ADR-0698](../adr/0698-vmafx-production-dockerfile.md) — production Dockerfile design
- [ADR-0798](../adr/0798-supply-chain-hardening.md) — supply-chain hardening policy
- [ADR-0703](../adr/0703-vmafx-server-go-grpc.md) — vmafx-controller Go service
- [ADR-0711](../adr/0711-vmafx-controller-impl.md) — controller implementation scope
