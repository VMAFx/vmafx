Add VMAFX production multi-arch Docker images with image signing and SBOM.

Introduces `docker/Dockerfile.production` (CPU-only, distroless, dual-target: `cli` +
`server`) and `docker/Dockerfile.production-gpu` (GPU-augmented variants for CUDA 12,
ROCm 6, oneAPI 2026, and Vulkan). Both produce `gcr.io/distroless/cc-debian12`-based
runtime images with no shell and minimal attack surface.

A new GitHub Actions workflow (`docker-publish-production.yml`) fires on every release
tag (`v*`) and builds amd64 + arm64 for the CPU, Vulkan, and server variants; amd64-only
for GPU SDK variants. All images are signed via Sigstore keyless cosign and carry a
CycloneDX SBOM attached as a cosign attestation.

Tag matrix: `latest` / `vX.Y.Z-lusoris.N` (CPU CLI), `-server`, `-cuda12`, `-rocm6`,
`-oneapi2026`, `-vulkan`. See `docs/development/docker-production.md` and ADR-0698.
