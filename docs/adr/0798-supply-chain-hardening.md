# ADR-0798: Container supply-chain hardening

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `ci`, `security`, `docker`, `supply-chain`

## Context

As VMAFX production images (`ghcr.io/vmafx/vmafx`, `ghcr.io/vmafx/vmafx-controller`)
are deployed to Kubernetes clusters by external operators, a compromised or tampered
image could introduce malicious code into scoring pipelines that process commercially
sensitive content. The project needed a verifiable chain of custody from source commit
to running container.

The following gaps existed before this ADR:

1. Runtime images used `debian:12-slim` (shell-present) rather than distroless, leaving
   an unnecessary attack surface at runtime.
2. CUDA, ROCm, and oneAPI GPU images were signed but lacked SBOM attestations.
3. No Trivy CVE scan gate existed; HIGH/CRITICAL CVEs could silently ship.
4. The `vmafx-controller` image had no CI publish job; the Dockerfile existed but
   was never pushed automatically.
5. `security-scans.yml` contained an unresolved merge conflict marker that broke
   the CodeQL Python job.
6. `docker/Dockerfile.production` referenced `libvmaf` (pre-rename path) in a
   `meson setup` invocation; the correct source directory is `core/`.

The project's SLSA aspiration is Build Level 3 for release binaries (covered by the
existing `supply-chain.yml`) and cosign keyless signing + SBOM for all container
images.

## Decision

We will apply the following hardening layer to every production container image
in the same CI workflow (`docker-publish-production.yml`):

1. **Distroless runtime** — all final runtime stages use
   `gcr.io/distroless/cc-debian12` (pinned by SHA-256). Build stages remain on
   `debian:12-slim` or `debian:13-slim` for toolchain availability.
2. **Multi-arch build matrix** — CPU and server images: `linux/amd64` + `linux/arm64`.
   GPU variants where the SDK is not available for arm64 remain amd64-only.
3. **Cosign keyless signing** — every pushed digest is signed via Sigstore OIDC
   immediately after `docker/build-push-action` reports the digest.
4. **SBOM via syft (CycloneDX JSON) + cosign attest** — every image variant gets
   an SBOM attached as a cosign attestation. GPU variant SBOMs use
   `continue-on-error: true` because the GPU SDK layers are large and syft
   may time out on free runners; the gate is best-effort for GPU variants only.
   CPU, server, and controller SBOM generation is required (no `continue-on-error`).
5. **Trivy scan** — after `build-cpu` pushes the primary digest, a `trivy-scan` job
   pulls the image and scans for OS + library CVEs. `exit-code: 1` on `HIGH` or
   `CRITICAL` unfixed findings blocks the `all-images` summary job.
   SARIF output is uploaded to the GitHub Security tab.
6. **Controller image CI publish** — a `build-controller` job (amd64 + arm64) now
   builds, pushes, signs, and attaches SBOM for
   `ghcr.io/vmafx/vmafx-controller`.

Fixes also applied in this ADR:
- Resolved merge conflict in `.github/workflows/security-scans.yml` (CodeQL Python job).
- Fixed stale `meson setup /build libvmaf` → `meson setup /build core` in
  `docker/Dockerfile.production`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Debian slim runtime (with shell) | Easier debugging | Shell is an attacker entry point; attack surface far larger | Distroless is strictly better for production |
| Long-lived cosign keypair | No OIDC infrastructure needed | Key management overhead; key rotation risk; worse auditability | Keyless OIDC is GitHub-native and already in use for binary artifacts |
| Grype instead of Trivy | Same vulnerability DB coverage | Trivy is already in use in `aquasecurity/trivy-action`; no ecosystem gain | Consistency with existing tooling |
| SLSA L3 for containers via `slsa-github-generator` | Stronger provenance | Container SLSA L3 via the generator is complex; cosign sign + SBOM attest meets the project's stated "Build L3 aspirational" posture for images | Too heavyweight for the current risk model |
| Block all GPU-variant SBOM failures | Maximum guarantees | Free runners time out on large GPU images; would cause spurious build failures | `continue-on-error: true` preserves the gate for CPU/server/controller while avoiding flakiness |

## Consequences

- **Positive**: every deployed image digest is independently verifiable via
  `cosign verify` + `cosign verify-attestation`. Trivy blocks HIGH/CRITICAL CVE
  shipments. Supply-chain provenance is auditable end-to-end.
- **Negative**: `trivy-scan` adds ~10 min to the release pipeline. GPU-variant
  SBOM generation is best-effort; operators of GPU images should run local
  `trivy image` scans before deploying.
- **Neutral / follow-ups**:
  - Renovate Bot should be configured to auto-update `gcr.io/distroless/cc-debian12`
    digest pins when Google releases a new distroless image.
  - A periodic Trivy sweep of GPU-variant images should be added to `nightly.yml`.

## References

- ADR-0698: vmafx-production-dockerfile (origin of production images).
- ADR-0703: vmafx-server Go gRPC service.
- ADR-0711: vmafx-controller Phase 4b.1 scope.
- [SLSA Build Level 3](https://slsa.dev/spec/v1.0/levels#build-l3)
- [Sigstore cosign documentation](https://docs.sigstore.dev/cosign/overview/)
- [Anchore syft](https://github.com/anchore/syft)
- [Aqua Security Trivy](https://aquasecurity.github.io/trivy/)
- [gcr.io/distroless project](https://github.com/GoogleContainerTools/distroless)
- Source: req — "Per ADR-0698 / PR #36, production images use distroless. Verify hardening + supply chain."
