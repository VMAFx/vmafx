<!-- markdownlint-disable MD013 MD060 -->
# VMAFX Production Docker Images

This page covers pulling, running, and building the VMAFX production container images
hosted at `ghcr.io/vmafx/vmafx`.

> For the **development MCP container** (full GPU toolchain, oneAPI, CUDA, HIP, MCP
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

The CPU CLI uses `gcr.io/distroless/cc-debian13:nonroot`, matching its Debian 13
builder ABI. The server uses the official Python 3.14 slim image (also Debian 13)
because a virtualenv requires its matching interpreter and standard library. GPU
variants use their vendors' pinned runtime families so the complete accelerator
runtime stays aligned with the compiler that produced `libvmaf`.

## Recovering a published image set

Manual publication is an idempotent recovery path for an existing published
ordinary-SemVer release, not a way to mint an arbitrary image tag from a branch.
Run the workflow at the same immutable tag passed as input:

```bash
tag=vX.Y.Z
gh workflow run docker-publish-production.yml --ref "$tag" -f tag="$tag"
```

The preflight rejects prereleases, unpublished tags, a dispatch ref other than
`refs/tags/$tag`, a source SHA mismatch, or coordinated version drift before
granting package-write or OIDC permissions.

## GPU variants

### CUDA 13.3.1

```bash
docker pull ghcr.io/vmafx/vmafx:vX.Y.Z-cuda13
docker run --rm --gpus all \
  ghcr.io/vmafx/vmafx:vX.Y.Z-cuda13 \
  --version
```

Requires the NVIDIA Container Toolkit and a host driver compatible with CUDA 13.3.1.

### ROCm 7.2.4 (HIP)

```bash
docker pull ghcr.io/vmafx/vmafx:vX.Y.Z-rocm7
docker run --rm \
  --device /dev/kfd \
  --device /dev/dri \
  --group-add video \
  --group-add render \
  ghcr.io/vmafx/vmafx:vX.Y.Z-rocm7 \
  --version
```

Requires: amdgpu kernel module loaded and `/dev/kfd` + `/dev/dri/renderD<N>` accessible.

### oneAPI 2025.3.1 (SYCL / Intel Arc)

```bash
docker pull ghcr.io/vmafx/vmafx:vX.Y.Z-oneapi2025
docker run --rm \
  --device /dev/dri \
  --group-add render \
  ghcr.io/vmafx/vmafx:vX.Y.Z-oneapi2025 \
  --version
```

Requires: `i915` or `xe` kernel module loaded and `/dev/dri/renderD<N>` accessible.

## MCP server variant

The `-server` tag starts the vmaf-mcp JSON-RPC server on port 8080:

```bash
docker run --rm -p 8080:8080 \
  -e VMAFX_MCP_HTTP_TOKEN='replace-with-a-secret' \
  ghcr.io/vmafx/vmafx:vX.Y.Z-server
```

The image explicitly binds `0.0.0.0` so its published port is reachable; the
HTTP transport otherwise defaults to loopback. Authentication remains
fail-closed. Include the configured bearer token in health, metrics, and score
requests. For a local-only disposable smoke, `VMAFX_MCP_HTTP_NO_AUTH=1` is the
explicit opt-out.

To override the port or run the stdio transport:

```bash
docker run --rm -p 8080:8080 \
  -e VMAFX_MCP_HTTP_TOKEN='replace-with-a-secret' \
  ghcr.io/vmafx/vmafx:vX.Y.Z-server \
  --transport http --port 8080

docker run --rm -i \
  --entrypoint /venv/bin/vmaf-mcp \
  ghcr.io/vmafx/vmafx:vX.Y.Z-server \
  --transport stdio
```

For the full vmaf-mcp environment variable reference see [docs/mcp/](../mcp/).

## Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `VMAF_MODEL_PATH` | `/usr/local/share/vmafx/model` | Directory searched for `.json` model files |
| `LD_LIBRARY_PATH` | `/usr/local/lib` | Path containing `libvmaf.so` |
| `VMAF_BINARY` | `/usr/local/bin/vmaf` | (server only) vmaf binary path for vmaf-mcp |
| `VMAFX_MCP_HTTP_BIND` | `0.0.0.0` in the server image | HTTP bind address; host installs default to `127.0.0.1` |
| `VMAFX_MCP_HTTP_TOKEN` | unset (fail closed) | Bearer token required by HTTP requests |

## Verifying image provenance

Every image is signed via Sigstore keyless cosign and carries a CycloneDX SBOM
attestation. Verify before deploying in a security-sensitive context:

```bash
# Verify the cosign signature
cosign verify \
  --certificate-identity-regexp "https://github.com/VMAFx/vmafx/.github/workflows/docker-publish-production.yml@.*" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  ghcr.io/vmafx/vmafx:latest

# Verify and print the SBOM attestation
cosign verify-attestation \
  --certificate-identity-regexp "https://github.com/VMAFx/vmafx/.github/workflows/docker-publish-production.yml@.*" \
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

# GPU variant (CUDA example)
docker buildx build \
  --platform linux/amd64 \
  --target final-cuda13 \
  -f docker/Dockerfile.production-gpu \
  -t vmafx:test-cuda13 \
  .
```

## Architecture notes

Both Dockerfiles use a multi-stage build:

1. **CPU builder** (`debian:13-slim`): compiles libvmaf + vmaf CLI with
   Meson/Ninja as a stripped release build.
2. **Python dependency builder** (`python:3.14-slim`, Debian 13): installs
   `vmaf-mcp` and `vmaf-tune` into `/venv`.
3. **CPU CLI runtime** (`gcr.io/distroless/cc-debian13:nonroot`): carries only
   the compiled binary, shared libraries, and model files.
4. **Server runtime** (the same pinned `python:3.14-slim` image): provides the
   interpreter to which `/venv/bin/python` links. It runs as UID/GID 65532.
5. **GPU builders/runtimes**: CUDA 13.3.1 uses NVIDIA devel/runtime images,
   ROCm 7.2.4 uses AMD's supported dev/application image, and oneAPI 2025.3.1
   uses Intel basekit/runtime images. Every reference is digest-pinned.

Publishing a GitHub release drives the two Docker workflows through the
`release.published` event. Each workflow checks out
`github.event.release.tag_name` and uses that same value for every image tag,
so a release cannot accidentally publish a branch tip under a release tag.
After each GPU image is signed and receives its SBOM and provenance, the
workflow verifies the digest-pinned signature before pulling the image and
runs the driver-independent `vmaf --version` entrypoint. This catches missing
runtime libraries without requiring accelerator hardware on the smoke runner.

See [ADR-0698](../adr/0698-vmafx-production-dockerfile.md) for the full rationale,
alternatives considered, and tag matrix design decisions.
