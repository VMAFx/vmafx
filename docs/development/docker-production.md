<!-- markdownlint-disable MD013 MD060 -->
# VMAFX Production Docker Images

This page covers pulling, running, and building the VMAFX production container images
hosted at `ghcr.io/vmafx/vmafx`.

> For the **development MCP container** (full GPU toolchain, oneAPI, CUDA, Vulkan, MCP
> server pre-installed), see [docs/development/dev-mcp.md](dev-mcp.md). That container
> is separate from the production images described here.

## Quick start

```bash
# Pull and run the vmaf CLI (CPU, smallest image)
docker pull ghcr.io/vmafx/vmafx:latest
docker run --rm ghcr.io/vmafx/vmafx:latest --version

# Score a video pair (mount a local directory)
docker run --rm \
  -v /path/to/videos:/data:ro \
  ghcr.io/vmafx/vmafx:latest \
  --reference /data/ref.yuv \
  --distorted /data/dis.yuv \
  --width 576 --height 324 \
  --pixel_format yuv420p \
  --bitdepth 8 \
  --model path=/usr/local/share/vmafx/model/vmaf_v0.6.1.json \
  --output /dev/stdout
```

## Tag matrix

| Tag | Platforms | Description | Approx. size |
|-----|-----------|-------------|--------------|
| `latest`, `vX.Y.Z` | amd64, arm64 | CPU-only CLI (default) | ~150 MB |
| `vX.Y.Z-server` | amd64, arm64 | CPU CLI + vmaf-mcp MCP server + vmaf-tune | ~350 MB |
| `vX.Y.Z-cuda13` | amd64 | CUDA 13 runtime added | ~500 MB |
| `vX.Y.Z-rocm7` | amd64 | ROCm 7 HIP runtime added | ~600 MB |
| `vX.Y.Z-oneapi2025` | amd64 | Intel oneAPI 2025 SYCL runtime added | ~500 MB |

All images are built from `gcr.io/distroless/cc-debian12` — no shell, minimal glibc
runtime. The attack surface is intentionally constrained.

## GPU variants

### CUDA 12

```bash
docker pull ghcr.io/vmafx/vmafx:latest-cuda12
docker run --rm --gpus all \
  ghcr.io/vmafx/vmafx:latest-cuda12 \
  --version
```

Requires: NVIDIA Container Toolkit installed on the host and an NVIDIA GPU with driver
compatible with CUDA 12 (driver >= 525.85).

### ROCm 6 (HIP)

```bash
docker pull ghcr.io/vmafx/vmafx:latest-rocm6
docker run --rm \
  --device /dev/kfd \
  --device /dev/dri \
  ghcr.io/vmafx/vmafx:latest-rocm6 \
  --version
```

Requires: amdgpu kernel module loaded and `/dev/kfd` + `/dev/dri/renderD<N>` accessible.

### oneAPI 2026 (SYCL / Intel Arc)

```bash
docker pull ghcr.io/vmafx/vmafx:latest-oneapi2026
docker run --rm \
  --device /dev/dri \
  ghcr.io/vmafx/vmafx:latest-oneapi2026 \
  --version
```

Requires: `i915` or `xe` kernel module loaded and `/dev/dri/renderD<N>` accessible.

### Vulkan

```bash
docker pull ghcr.io/vmafx/vmafx:latest-vulkan
docker run --rm \
  --device /dev/dri \
  -v /usr/share/vulkan/icd.d:/usr/share/vulkan/icd.d:ro \
  ghcr.io/vmafx/vmafx:latest-vulkan \
  --version
```

Mount the host's Vulkan ICD directory so the loader can find your GPU's ICD JSON.
On hosts without a GPU, lavapipe (software Vulkan) can be used instead.

## MCP server variant

The `-server` tag starts the vmaf-mcp JSON-RPC server on port 8080:

```bash
docker run --rm -p 8080:8080 \
  ghcr.io/vmafx/vmafx:latest-server
```

To override the transport or bind address:

```bash
docker run --rm -p 8080:8080 \
  -e VMAFX_MCP_HOST=0.0.0.0 \
  -e VMAFX_MCP_PORT=8080 \
  ghcr.io/vmafx/vmafx:latest-server \
  --transport http --host 0.0.0.0 --port 8080
```

For the full vmaf-mcp environment variable reference see [docs/mcp/](../mcp/).

## Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `VMAF_MODEL_PATH` | `/usr/local/share/vmafx/model` | Directory searched for `.json` model files |
| `LD_LIBRARY_PATH` | `/usr/local/lib` | Path containing `libvmaf.so` |
| `VMAF_BINARY` | `/usr/local/bin/vmaf` | (server only) vmaf binary path for vmaf-mcp |

## Verifying image provenance

Every image is signed via Sigstore keyless cosign and carries a CycloneDX SBOM
attestation. Verify before deploying in a security-sensitive context:

```bash
# Verify the cosign signature
cosign verify \
  --certificate-identity-regexp "https://github.com/VMAFx/vmafx/.github/workflows/docker-publish-production.yml" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  ghcr.io/vmafx/vmafx:latest

# Verify and print the SBOM attestation
cosign verify-attestation \
  --certificate-identity-regexp "https://github.com/VMAFx/vmafx/.github/workflows/docker-publish-production.yml" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  --type cyclonedx \
  ghcr.io/vmafx/vmafx:latest \
  | jq '.payload | @base64d | fromjson'
```

## Building locally

```bash
# CPU CLI (default)
docker buildx build \
  --platform linux/amd64 \
  --target cli \
  -f docker/Dockerfile.production \
  -t vmafx:test-cli \
  .

# Server
docker buildx build \
  --platform linux/amd64 \
  --target server \
  -f docker/Dockerfile.production \
  -t vmafx:test-server \
  .

# GPU variant (Vulkan example)
docker buildx build \
  --platform linux/amd64 \
  --target final-vulkan \
  -f docker/Dockerfile.production-gpu \
  -t vmafx:test-vulkan \
  .
```

## Architecture notes

Both Dockerfiles use a multi-stage build:

1. **builder** (`ubuntu:26.04`): compiles libvmaf + vmaf CLI with meson/ninja.
   Release build, stripped, no GPU backends in the default Dockerfile.
2. **python-deps** (`python:3.13-slim`): installs vmaf-mcp and vmaf-tune into
   `/venv` (server target only).
3. **runtime** (`gcr.io/distroless/cc-debian12`): copies only the compiled
   binary, shared libraries, and model files. No shell, no package manager.

The GPU Dockerfile adds GPU-specific library layers on top of the runtime stage by
copying from their upstream base images rather than installing full SDKs — this keeps
GPU variant image sizes manageable.

See [ADR-0698](../adr/0698-vmafx-production-dockerfile.md) for the full rationale,
alternatives considered, and tag matrix design decisions.
