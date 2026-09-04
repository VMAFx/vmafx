<!-- markdownlint-disable MD013 MD060 -->
# Research Digest: Dev Container Image Publication and Runner Disk Impact

- **Date**: 2026-09-04
- **Related ADR**: [ADR-1178](../adr/1178-dev-container-image-publish.md)
- **Author**: Lusoris

## 1. Context and Problem Statement

Under [ADR-1102](../adr/1102-phase4b9-container-only-publishing.md), all canonical build artifacts (release binaries, published images, CI artifacts consumed downstream) must be produced inside the `vmaf-dev-mcp` container. However, `.github/workflows/supply-chain.yml` historically built native release artifacts (`libvmaf.so` SONAME chain, `vmaf` CLI binary, `models.tar.gz`) directly on the bare `ubuntu-latest` runner host using ad-hoc `apt-get` and PyPI `meson`.

The container-build detector `scripts/ci/check-container-build.sh` enforces this policy by asserting containerness via `/etc/vmafx-dev-container` and stamping staged artifact trees (`container-build-provenance.txt`). To run `build-artifacts` inside the canonical container in GitHub Actions, the container image must be pre-published and accessible from GHCR (`ghcr.io/vmafx/vmafx-dev-mcp`).

This research digest evaluates the image dimensions, layer composition, pull times, and runner disk space implications of using the canonical dev container in the release pipeline.

## 2. Image Metrics and Layer Breakdown

Local inspection of the canonical dev container (`vmaf-dev-mcp:local`, image digest `sha256:ff297e6d26bbcfaa094dba593984822f003ebc1bf60d2b07d67f90533acacf70`):

| Component / Layer | Approximate Uncompressed Size | Notes |
|---|---|---|
| Base OS (`ubuntu:24.04`) + build tools (`gcc-15`, `clang`, `ninja`, `meson`) | ~1.2 GB | System packages, headers, Python runtime |
| NVIDIA CUDA Toolkit 13.3 + NVCodec headers | ~6.2 GB | CUDA compiler, runtime, libraries |
| Intel oneAPI 2025.3 (DPC++/C++, Level Zero) | ~8.4 GB | oneAPI Base Toolkit, SYCL toolchain |
| AMD ROCm 7.2.4 (HIP runtime, rocBLAS) | ~18.5 GB | HIP compiler and device libraries |
| ONNX Runtime 1.29.0 + Python dependencies | ~3.8 GB | Inference engine, numpy, scipy |
| Libvmaf build stage (`libvmaf-build` target) | **~45.4 GB total** | Cumulative stage used for compilation |
| Downstream stages (FFmpeg n9.0.1, Go MCP server) | ~6.9 GB | Excluded when targeting `libvmaf-build` |
| Full development image (`dev-mcp` target) | ~52.3 GB total | Full interactive dev environment |

### Transfer vs. Storage Sizing

- **Virtual uncompressed footprint (`libvmaf-build`)**: ~45.4 GB
- **Compressed registry transfer size (zstd / gzip blobs)**: ~10.5 - 11.6 GB
- **Pull time on GitHub Actions network**: ~2 to 4 minutes at typical runner network speeds (400-800 Mbps).

## 3. Runner Disk Footprint and Headroom

GitHub-hosted `ubuntu-latest` (Standard 2-core x86_64 runner) characteristics:

- Total root disk space: ~75 GB.
- Pre-installed software (Android SDKs, .NET, Haskell, Docker images): ~30-35 GB.
- Default available free space: ~35-42 GB.

### Disk Headroom Analysis

1. **Targeting `libvmaf-build`**:
   The publish workflow `.github/workflows/dev-container-publish.yml` targets `libvmaf-build`, eliminating ~7 GB of FFmpeg build trees, Go toolchains, and MCP server assets from the image.
2. **Container Runner Cache**:
   In `supply-chain.yml`, GitHub Actions runs the job inside `container: { image: ... }`. The runner daemon pulls layers sequentially. Because Docker unpacks layers as it pulls, disk usage peaks during layer extraction.
3. **Build Space Safety**:
   Native artifact compilation (`meson setup build core --buildtype=release ... && meson compile -C build`) generates ~150 MB of build objects and ~25 MB of staged artifacts (`libvmaf.so`, `vmaf`, `models.tar.gz`).
4. **Remediation if Free Space Regresses**:
   If future GPU SDK updates expand the layer size beyond standard runner free space, two standard mitigations exist:
   - Run a disk-clearing step before container pull (e.g. removing `/usr/local/lib/android` and `/opt/ghc`, which frees ~25 GB in 15 seconds).
   - Alternatively, dispatch `build-artifacts` on larger runners or self-hosted runners where disk limits are configurable.
   Currently, the 25-minute `timeout-minutes` ceiling in `build-artifacts` is more than sufficient for the ~3 minute pull and ~45 second compilation.

## 4. Decision and Operational Verification

- **Chosen Approach**: Option (a) — Publish canonical container image to GHCR via `dev-container-publish.yml` on pushes to `master` affecting `dev/Containerfile` or `dev/scripts/**`, with digest pinning and cosign keyless signing.
- **Verification Gate**: `scripts/ci/check-container-build.sh --stamp artifacts` inside the container build step, verified by `scripts/ci/check-container-build.sh --verify artifacts` in downstream verification and release attachment gates.
