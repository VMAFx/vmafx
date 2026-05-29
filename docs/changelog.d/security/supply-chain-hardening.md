# Container supply-chain hardening (ADR-0798)

All VMAFX production images now ship with a complete supply-chain provenance story:

- **Distroless runtime** (`gcr.io/distroless/cc-debian12`) for all image targets: `cli`, `server`, `cuda12`, `rocm6`, `oneapi2026`, `vulkan`, `controller`.
- **Multi-arch build matrix**: CPU and server images are published for `linux/amd64` + `linux/arm64`; GPU variants remain amd64-only where the SDK is not available for arm64.
- **Cosign keyless signing** (Sigstore OIDC) for every pushed image digest.
- **syft SBOM** (CycloneDX JSON) generated and attached as a cosign attestation for all image variants.
- **Trivy CVE scan** gating the CPU image on HIGH/CRITICAL unpatched findings; SARIF uploaded to the GitHub Security tab.
- **`vmafx-controller` image** is now built, pushed, signed, and SBOM-attested by CI (`build-controller` job in `docker-publish-production.yml`).
- Resolved merge conflict in `.github/workflows/security-scans.yml` (CodeQL Python job).
- Fixed stale `meson setup /build libvmaf` → `meson setup /build core` in `docker/Dockerfile.production`.

Verify an image with:
```
cosign verify \
  --certificate-identity-regexp "https://github.com/lusoris/vmaf/.github/workflows/docker-publish-production.yml" \
  --certificate-oidc-issuer "https://token.actions.githubusercontent.com" \
  ghcr.io/vmafx/vmafx:<tag>
```

See `docs/k8s/supply-chain.md` for full verification instructions.
