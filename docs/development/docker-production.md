<!-- markdownlint-disable MD013 MD060 -->
# VMAFX Production Docker Images

This page covers pulling, running, and building the VMAFX production container images
hosted at `ghcr.io/vmafx/vmafx`.

> For the **development MCP container** (full GPU toolchain, oneAPI, CUDA, HIP, MCP
> server pre-installed), see [docs/development/dev-mcp.md](dev-mcp.md). That container
> is separate from the production images described here.

## Quick start

Name the release you want. `latest` points at the newest final release only;
release candidates (`vX.Y.Z-rc.N`) are never tagged `latest`, so until 1.0.0
is out, `latest` does not exist.

```bash
tag=v1.0.0-rc.1

# Pull and run the vmaf CLI (CPU, smallest image)
docker pull ghcr.io/vmafx/vmafx:$tag
docker run --rm ghcr.io/vmafx/vmafx:$tag --version

# Score a video pair (mount a local directory)
docker run --rm \
  -v /path/to/videos:/data:ro \
  ghcr.io/vmafx/vmafx:$tag \
  --reference /data/ref.yuv \
  --distorted /data/dis.yuv \
  --width 576 --height 324 \
  --pixel_format 420 \
  --bitdepth 8 \
  --model path=/usr/local/share/vmafx/model/vmaf_v0.6.1.json \
  --output /dev/stdout
```

## Tag matrix

| Tag | Platforms | Description | Approx. size |
|-----|-----------|-------------|--------------|
| `vX.Y.Z` (also `latest` for a final release) | amd64, arm64 | CPU-only CLI (default) | ~144 MB unpacked, ~55 MB compressed |
| `vX.Y.Z-server` | amd64, arm64 | CPU CLI + vmaf-mcp MCP server + vmaf-tune | ~1.1 GB unpacked, ~283 MB compressed |
| `vX.Y.Z-cuda13` | amd64 | CUDA 13 runtime added | ~313 MB unpacked, ~100 MB compressed |
| `vX.Y.Z-rocm10` | amd64 | ROCm 10 HIP runtime added | ~29 GB unpacked, ~8.3 GB compressed |
| `vX.Y.Z-oneapi2025` | amd64 | Intel oneAPI 2025 SYCL runtime added | ~5.8 GB unpacked, ~1.4 GB compressed |

The CPU CLI uses `gcr.io/distroless/cc-debian13:nonroot`, matching its Debian 13
builder ABI. The server uses the official Python 3.14 slim image (also Debian 13)
because a virtualenv requires its matching interpreter and standard library. The
CUDA variant uses the same digest-pinned Ubuntu 26.04 base for its builder and
runtime and installs exact NVIDIA apt packages in each stage. The other GPU
variants use their vendors' pinned runtime families.

## Recovering a published image set

Manual publication is an idempotent recovery path for an existing published
release (final or candidate), not a way to mint an arbitrary image tag from a
branch. Run the workflow at the same immutable tag passed as input:

```bash
tag=vX.Y.Z
gh workflow run docker-publish-production.yml --ref "$tag" -f tag="$tag"
```

The preflight rejects unpublished tags, a prerelease flag that disagrees with
the tag, a dispatch ref other than `refs/tags/$tag` or `master`, a source SHA
mismatch, or coordinated version drift before granting package-write or OIDC
permissions. A dispatch on `master` is the recovery for a broken build recipe:
it builds the tag's source with `master`'s build recipe (`docker/`,
`Dockerfile.go-server`, `ffmpeg-patches/`) and signs as `master`; see
[Recovering a release's container images](release.md#recovering-a-releases-container-images).

## GPU variants

`vmaf --version` never touches the GPU, so it does not show that an image can
use one. Check with a scoring run that forces the backend: with
`--backend cuda`, `hip` or `sycl`, a backend that cannot initialise makes the
CLI exit with code `100` instead of scoring on the CPU (see
[explicit-backend semantics](../backends/index.md#explicit-backend-semantics-backend-name)).
The default, `--backend auto`, falls back to the CPU with only a message on
stderr, so a container started without GPU access still prints a score.

The examples score the pair in `/path/to/videos` with `tag` set as in the
[quick start](#quick-start), and pass the model by path, which works in
every image.

### CUDA 13.4.2

```bash
docker run --rm --gpus all \
  -v /path/to/videos:/data:ro \
  ghcr.io/vmafx/vmafx:$tag-cuda13 \
  --backend cuda \
  --reference /data/ref.yuv --distorted /data/dis.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --model path=/usr/local/share/vmafx/model/vmaf_v0.6.1.json \
  --output /dev/stdout
```

Requires the NVIDIA Container Toolkit and a host driver compatible with CUDA 13.4.2.
Without `--gpus all` the container has no GPU: `--backend cuda` exits with
code `100`, and the default auto mode scores on the CPU.

### ROCm 10.0.0 (HIP)

Pass `/dev/kfd` and the render node of the GPU to use, found from its PCI
address under `/dev/dri/by-path/`. Passing all of `/dev/dri` also works, but
exposes every GPU in the host. Add the host's `render` and `video` groups by
numeric ID: the image has no `render` group, so `--group-add render` fails
with `unable to find group render`, and `--group-add video` resolves to the
image's GID 44 rather than the host's `video` group.

```bash
# The GPU's PCI address; list them with: ls -l /dev/dri/by-path/
render=$(readlink -f /dev/dri/by-path/pci-0000:7d:00.0-render)
docker run --rm \
  --device /dev/kfd \
  --device "$render" \
  --group-add "$(getent group render | cut -d: -f3)" \
  --group-add "$(getent group video | cut -d: -f3)" \
  -v /path/to/videos:/data:ro \
  ghcr.io/vmafx/vmafx:$tag-rocm10 \
  --backend hip \
  --reference /data/ref.yuv --distorted /data/dis.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --model path=/usr/local/share/vmafx/model/vmaf_v0.6.1.json \
  --output /dev/stdout
```

Requires: amdgpu kernel module loaded and `/dev/kfd` + `/dev/dri/renderD<N>` accessible.

### oneAPI 2025.3 (SYCL / Intel Arc)

The image is compiled in Intel's oneAPI Base Toolkit container tagged 2025.3.2
and runs on Intel's oneAPI Runtime 2025.3.1 image. Intel has not published a matching
`oneapi-runtime:2025.3.2-0-devel-ubuntu24.04` tag, so the release keeps the
runtime on the latest available 2025.3 patch.

`vmaf --version` loads only the libraries `vmaf` links directly. It does not
load the oneAPI Unified Runtime adapters, which SYCL opens with `dlopen()`
when it looks for a device. The `v1.0.0-rc.1` image passed `--version` while
every adapter failed to load for want of `libumf.so.1`: SYCL reported "No
device of requested type available" and `--backend sycl` exited with code
`100`. The publication smoke test now also checks the adapters with `ldd`.

Pass the render node and the host's `render` group by numeric ID, as for
ROCm:

```bash
# The GPU's PCI address; list them with: ls -l /dev/dri/by-path/
render=$(readlink -f /dev/dri/by-path/pci-0000:03:00.0-render)
docker run --rm \
  --device "$render" \
  --group-add "$(getent group render | cut -d: -f3)" \
  -v /path/to/videos:/data:ro \
  ghcr.io/vmafx/vmafx:$tag-oneapi2025 \
  --backend sycl \
  --reference /data/ref.yuv --distorted /data/dis.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --model path=/usr/local/share/vmafx/model/vmaf_v0.6.1.json \
  --output /dev/stdout
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
tag=v1.0.0-rc.1
# The signing identity is the workflow at the ref it ran on: refs/tags/$tag for
# a normal publish, refs/heads/master for a recovered image (every
# v1.0.0-rc.1 image; see release.md). Pin the one that applies.
identity="https://github.com/VMAFx/vmafx/.github/workflows/docker-publish-production.yml@refs/heads/master"

# Verify the cosign signature
cosign verify \
  --certificate-identity "$identity" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  ghcr.io/vmafx/vmafx:$tag

# Verify and print the SBOM attestation
cosign verify-attestation \
  --certificate-identity "$identity" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  --type cyclonedx \
  ghcr.io/vmafx/vmafx:$tag \
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
5. **GPU builders/runtimes**: CUDA 13.4.2 uses the same digest-pinned Ubuntu 26.04
   base for its builder and runtime, installing exact NVIDIA apt packages in each
   stage. ROCm 10.0.0 uses AMD's `rocm/dev-ubuntu-26.04:10.0.0-full` image for
   both its builder and runtime, and the Intel image uses the oneAPI basekit
   image tagged 2025.3.2 with the latest published 2025.3.1 runtime. Every
   base-image reference is digest-pinned.

Publishing a GitHub release drives the two Docker workflows through the
`release.published` event. Each workflow checks out
`github.event.release.tag_name` and uses that same value for every image tag,
so a release cannot accidentally publish a branch tip under a release tag.
After each GPU image is signed and receives its SBOM and provenance, the
workflow verifies the digest-pinned signature before pulling the image and
runs `vmaf --version`, which needs no accelerator hardware. That proves only
that the libraries `vmaf` links directly are present. Runtime plugins opened
with `dlopen()` are not loaded until a backend initialises, so for the oneAPI
image the smoke also runs `ldd` on the Unified Runtime adapters. Neither check
exercises a GPU; run a forced-backend score from
[GPU variants](#gpu-variants) on the target hardware for that.

See [ADR-0698](../adr/0698-vmafx-production-dockerfile.md) for the full rationale,
alternatives considered, and tag matrix design decisions.
