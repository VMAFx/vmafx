<!-- markdownlint-disable MD013 MD060 -->
# VMAFX Production Docker Images

This page covers pulling, running, and building the VMAFX production container
images hosted at `ghcr.io/vmafx/vmafx`.

!!! note
    For the **development MCP container** (full GPU toolchain, oneAPI, CUDA,
    HIP, MCP server pre-installed), see [dev-mcp.md](dev-mcp.md). That
    container is separate from the production images described here, and it
    carries a different MCP server: the dev container runs the Go
    `vmafx-mcp`, while the `-server` image below still runs the Python
    `vmaf-mcp` (see [MCP server variant](#mcp-server-variant)).

## Quick start

Name the release you want. `latest` points at the newest final release only;
release candidates (`vX.Y.Z-rc.N`) are never tagged `latest`, so until 1.0.0
is out, `latest` does not exist. The examples use `v1.0.0-rc.2`; substitute the
newest release listed on the GitHub releases page.

Images published after `v1.0.0-rc.2` have zstd layers and need Docker Engine 23.0 or
later, Docker Desktop 4.19 or later, Podman or containerd 1.5 or later ([what can pull
them](../usage/docker.md#what-can-pull-the-images)).

```bash
tag=v1.0.0-rc.2

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
| `vX.Y.Z-cuda13` | amd64 | CUDA 13 build of the CLI; no NVIDIA library (the host driver provides `libcuda`) | ~234 MB unpacked, ~76 MB compressed |
| `vX.Y.Z-rocm10` | amd64 | HIP build of the CLI with the ROCm 10 runtime files it loads | ~611 MB unpacked, ~175 MB compressed |
| `vX.Y.Z-oneapi2026` (also `vX.Y.Z-oneapi2025`) | amd64 | SYCL build of the CLI with the oneAPI 2026.1 SYCL runtime files it loads and the Intel GPU compute runtime | ~590 MB unpacked, ~163 MB compressed |

### Base images

| Variant | Base image | Why |
|---------|------------|-----|
| CPU CLI | `gcr.io/distroless/cc-debian13:nonroot` | Matches its Debian 13 builder ABI. |
| Server | Official Python 3.14 slim image (also Debian 13) | A virtualenv requires its matching interpreter and standard library. |
| CUDA | `debian:13-slim`, the CPU image's builder base, for builder and runtime | The builder installs `nvcc` from NVIDIA's `debian13` repository; the runtime holds no NVIDIA file. |
| ROCm | `debian:13-slim` for builder and runtime | The builder streams `/opt/rocm` out of AMD's pinned `rocm/dev-ubuntu-26.04` image; the runtime holds only the HIP runtime files `vmaf` loads. |
| oneAPI | `debian:13-slim` for builder and runtime | The builder installs Intel's compiler at an exact apt build; the runtime holds the SYCL runtime files `vmaf` loads; see [oneAPI 2026.1](#oneapi-20261-sycl-intel-arc). |

The three GPU images carry no vendor toolchain: no `nvcc`, `hipcc` or
Intel runtime tree under `/opt/intel/oneapi`
([ADR-1517](../adr/1517-gpu-image-licensing.md)). The ROCm runtime files are in
`/usr/local/lib/rocm`, the Intel ones in `/usr/local/lib/intel`; each image's
`LD_LIBRARY_PATH` names them.

### oneAPI tag names

Releases up to v1.0.0-rc.2 published the oneAPI image only as `-oneapi2025`.
Later releases publish the same image under both `-oneapi2026` and
`-oneapi2025`, so scripts that name the old suffix keep working. Use
`-oneapi2026` in new scripts: the older suffix is kept for compatibility and
will be retired only by a release that announces a breaking change.

## Recovering a published image set

Manual publication is an idempotent recovery path for an existing published
release (final or candidate), not a way to mint an arbitrary image tag from a
branch. Run the workflow at the same immutable tag passed as input:

```bash
tag=vX.Y.Z
gh workflow run docker-publish-production.yml --ref "$tag" -f tag="$tag"
```

The preflight rejects, before granting package-write or OIDC permissions:

- an unpublished tag;
- a prerelease flag that disagrees with the tag;
- a dispatch ref other than `refs/tags/$tag` or `master`;
- a source SHA mismatch;
- coordinated version drift.

A dispatch on `master` is the recovery for a broken build recipe: it builds
the tag's source with `master`'s build recipe (`docker/`,
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

Requires the NVIDIA Container Toolkit and a host driver compatible with
CUDA 13.4.2. The image holds no NVIDIA library: `libvmaf` loads the host
driver's `libcuda.so.1`, which the toolkit mounts. Without `--gpus all` the
container has no GPU: `--backend cuda` exits with code `100`, and the default
auto mode scores on the CPU.

### ROCm 10.1.0 (HIP)

Pass `/dev/kfd` and the render node of the GPU to use, found from its PCI
address under `/dev/dri/by-path/`. Passing all of `/dev/dri` also works, but
exposes every GPU in the host.

Add the host's `render` and `video` groups by numeric ID:

- The image has no `render` group, so `--group-add render` fails with
  `unable to find group render`.
- `--group-add video` resolves to the image's GID 44 rather than the host's
  `video` group.

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

Requires: amdgpu kernel module loaded and `/dev/kfd` + `/dev/dri/renderD<N>`
accessible. The HIP kernels cover every GPU target ROCm 10.1.0 builds its own
libraries for (the `dist_amdgpu_targets` list of ROCm's
`share/therock/dist_info.json`, 25 targets from `gfx908` to `gfx1250`).

### oneAPI 2026.1 (SYCL, Intel Arc)

The image is built and run on Debian 13, the base of the CPU image.
Everything Intel-specific is installed from pinned packages, with the versions
set in `build-config.env`:

| Component | Where it comes from | Pin |
|-----------|---------------------|-----|
| oneAPI DPC++/C++ compiler (builder) | Intel's oneAPI apt repository | `ONEAPI_APT_VERSION` (2026.1.1-325) |
| SYCL runtime: `libsycl`, the Unified Runtime loader and its Level Zero adapters, the compiler's maths and support libraries, UMF (`libumf.so.1`) and hwloc (image, `/usr/local/lib/intel`) | copied from the builder's compiler installation, every compiler file listed in its `credist.txt` (`tools/rc1-tester/image/sycl-runtime.json`) | `ONEAPI_APT_VERSION` |
| GPU compute runtime: Level Zero GPU driver, IGC, gmmlib | the `intel/compute-runtime` GitHub release | `INTEL_NEO_VERSION` (26.35.39758.10) |
| Level Zero loader (`libze_loader.so.1`) | the `oneapi-src/level-zero` GitHub release | `LEVEL_ZERO_VERSION` |

SYCL reaches the GPU through Level Zero only: the compute runtime's OpenCL ICD
and offline compiler are removed again after installation, as in the Intel GPU
tester image, so `ONEAPI_DEVICE_SELECTOR=opencl:*` finds no device.

The GPU compute runtime is the part the host does not provide: the host
supplies only the kernel driver (`i915` or `xe`) and the device node.

#### Run it

Pass the render node and the host's `render` group by numeric ID, as for
ROCm:

```bash
# The GPU's PCI address; list them with: ls -l /dev/dri/by-path/
render=$(readlink -f /dev/dri/by-path/pci-0000:03:00.0-render)
docker run --rm \
  --device "$render" \
  --group-add "$(getent group render | cut -d: -f3)" \
  -v /path/to/videos:/data:ro \
  ghcr.io/vmafx/vmafx:$tag-oneapi2026 \
  --backend sycl \
  --reference /data/ref.yuv --distorted /data/dis.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --model path=/usr/local/share/vmafx/model/vmaf_v0.6.1.json \
  --output /dev/stdout
```

Requires: `i915` or `xe` kernel module loaded and `/dev/dri/renderD<N>`
accessible. With more than one Intel GPU, pick one with
`ONEAPI_DEVICE_SELECTOR`, for example `-e ONEAPI_DEVICE_SELECTOR=level_zero:0`.

#### Windows (WSL 2)

On Windows, Docker Desktop's WSL 2 backend exposes the GPUs through
`/dev/dxg` instead of a render node, and the GPU driver's user-space half
lives in the host's `/usr/lib/wsl/lib`, which the image's library path already
names. Pass both:

```bash
docker run --rm \
  --device /dev/dxg \
  -v /usr/lib/wsl:/usr/lib/wsl:ro \
  -e ONEAPI_DEVICE_SELECTOR=level_zero:0 \
  -v /path/to/videos:/data:ro \
  ghcr.io/vmafx/vmafx:$tag-oneapi2026 \
  --backend sycl \
  --reference /data/ref.yuv --distorted /data/dis.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --output /dev/stdout
```

!!! note
    `vmaf --version` loads only the libraries `vmaf` links directly. It does
    not load the oneAPI Unified Runtime adapters, which SYCL opens with
    `dlopen()` when it looks for a device. The image build and the publication
    smoke test check the adapters with `ldd`; a forced `--backend sycl` score
    is the only check that exercises the device.

## MCP server variant

The `-server` tag starts the Python `vmaf-mcp` JSON-RPC server on port 8080
(entry point `/venv/bin/vmaf-mcp --transport http`). The Go server
`vmafx-mcp`, which [ADR-1229](../adr/1229-mcp-go-runtime.md) made the MCP
server in the dev container, is not in this image.

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
tag=vX.Y.Z
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

The CPU and server images also carry an SPDX SBOM of each platform image,
attested with GitHub's attestation store
([ADR-1513](../adr/1513-production-artifact-licensing.md)):

```bash
gh attestation verify oci://ghcr.io/vmafx/vmafx:$tag --repo VMAFx/vmafx \
  --predicate-type https://spdx.dev/Document/v2.3
```

## Licences and corresponding source

Every published target writes its notices into the image and cannot be built
without passing the licence check
([ADR-1513](../adr/1513-production-artifact-licensing.md), the rules of
[ADR-1503](../adr/1503-tester-artifact-licensing.md); GPU images
[ADR-1517](../adr/1517-gpu-image-licensing.md)):

| Path or tag | What it holds |
| --- | --- |
| `/usr/local/share/vmafx/licenses/THIRD_PARTY_NOTICES.txt` | every component of the image: VMAFx's compiled files with their licences and copyright lines, the models, CPython and every Python package (server), every Debian package with its source package |
| `/usr/local/share/vmafx/licenses/texts/` | the licence texts the notices name |
| `/usr/local/share/vmafx/licenses/vmafx-compiled-sources.json` | the licence of every repository file the build compiled |
| `/usr/local/share/vmafx/licence-check.json` | the receipt of the licence check |
| `ghcr.io/vmafx/vmafx:<tag>-source`, `<tag>-server-source`, `<tag>-cuda13-source`, `<tag>-rocm10-source`, `<tag>-oneapi2026-source` | the corresponding source of the copyleft parts: Debian source packages of every installed package at the installed version, (server) the GCC source RPMs of the runtimes grafted into the numpy and scipy wheels, and (ROCm) the elfutils and numactl archives and the TheRock tree that built the LGPL libraries of the ROCm runtime; `SOURCES.txt` is the index |

How the gate works, per stage of `docker/Dockerfile.production`:

1. `builder` runs `licensing.py scan-build`, which reads `ninja -t deps` and the
   SPDX headers (or `REUSE.toml`) of every compiled repository file.
2. `licence-texts` downloads the recorded texts that are not in the repository
   (CPython's `Doc/license.rst`, the pinned `fetched_texts`), each by SHA-256.
3. `cli-notices` (on a copy of the distroless tree, which has no interpreter) and
   `server-assembled` write the notices with `licensing.py notices`.
4. `cli-licence-check` / `server-licence-check` run `licensing.py check` on the
   finished tree: a file no component of `tools/rc1-tester/image/licensing.json`
   claims, a Debian package without its copyright file, a dist-info without a
   licence file, a copyleft wheel graft without recorded source, or a missing
   text fails the build. `cli` and `server` copy the check's receipt, so they
   cannot be built without it.
5. `cli-source-export` / `server-source-export` fetch the corresponding source
   (`licensing.py sources`, `fetch-sources`); the release workflow pushes them as
   the `-source` tags through `.github/actions/image-licence-artifacts`.

A base-image or lock bump that brings a new package, wheel library or licence
fails the release build until `licensing.json` records it. Run the check locally
the same way the release does:

```bash
docker buildx build --target cli -f docker/Dockerfile.production -t vmafx:test-cli .
docker buildx build --target cli-source-export -f docker/Dockerfile.production \
  --output type=local,dest=./cli-source .
```

### GPU images

`docker/Dockerfile.production-gpu` follows the same pattern per variant
([ADR-1517](../adr/1517-gpu-image-licensing.md)): `builder-<variant>` runs
`scan-build` and stages the vendor files the image ships into `/stage`
(`prepare_build.py rocm-runtime` / `intel-runtime` with the tester images'
`hip-runtime.json` / `sycl-runtime.json`; for CUDA only the EULA text and the
`nv-codec-headers` notices); `<variant>-notices` writes the notices on a copy of
the assembled tree; `<variant>-licence-check` runs the check (artifact kinds
`production-cuda-image`, `production-rocm-image`, `production-oneapi-image`) and
`final-<variant>` copies its receipt; `<variant>-source-export` holds the
source, published as `<tag>-<variant>-source`. The variants are `cuda13`,
`rocm10` and `oneapi2026`. The CUDA build also fails when a binary links an
NVIDIA library or an NVIDIA file is in the image. The three records take the
vendor components of the tester records by reference, so a vendor runtime is
recorded once for both images.

### Go service and node images

`docker/Dockerfile.operator`, `Dockerfile.go-server` and `docker/Dockerfile.node`
(`node-cpu`) follow the same pattern
([ADR-1514](../adr/1514-go-and-node-image-licensing.md)): the go-builder stage
runs `licensing.py scan-go` (the licences of our own Go files the program
compiles) and `licensing.py go-licences`, which reads the module list from the
binary's build information and copies every module's `LICENSE*`, `COPYING*`,
`NOTICE*` and `PATENTS*` files into `/usr/local/share/vmafx/licenses/go/`. The
licence check fails on a module without a text or with a licence it cannot
classify (`go_module_licences` in `licensing.json` records exceptions, such as a
nested module that shares its repository's `LICENSE`). The `source-export` /
`node-source-export` stages hold the module zips of every copyleft module,
checked against the binary's `h1:` sums, and the Debian sources of the base.

The node image adds:

- FFmpeg built with `--enable-gpl --enable-version3` (never `--enable-nonfree`),
  its licence files in `/usr/local/share/vmafx/ffmpeg/`, and in the source image
  the patched tree exactly as compiled with `CONFIGURE.txt` and the patch series;
- the libraries copied out of Debian packages for FFmpeg, recorded by
  `scripts/ci/record-copied-debian-libs.sh` in
  `/usr/local/share/vmafx/copied-packages/packages.list` with each package's
  copyright file (the source image holds their Debian sources);
- for rclone's mount mode, the setuid `fusermount3` (Debian `fuse3`,
  GPL-2.0) and the util-linux `mount` and `umount` it runs, with their
  libraries at their Debian paths, recorded the same way in
  `/usr/local/share/vmafx/fuse-tools/packages.list`
  ([ADR-1593](../adr/1593-helm-node-fuse-and-ebpf.md));
- rclone built from its release's module source at `RCLONE_VERSION`
  (`build-config.env`), so the source image can hold the exact source of it and
  of every module it links (it links an LGPL-3.0 module).

### Images published before these rules

The images of 1.0.0-rc.1 and rc.2 were published without notices or source
([ADR-1578](../adr/1578-published-rc-licence-companions.md)). The images that
stay are listed under `keep` in
`tools/rc1-tester/image/published-rc/artifacts.json`, each with its digest and
licence record (`published-rc-*` in `licensing.json`). The manual workflow
`.github/workflows/published-rc-licence-companions.yml` completes them:

1. It refuses an image whose tag no longer names the recorded digest.
2. It unpacks every platform of that digest (`docker export`) and writes its
   notices with `licensing.py notices`. The licence scan of the release tag's
   build is recorded in `published-rc/scans/<release>/<build>.json`, so the
   workflow compiles nothing.
3. It fetches the source of every installed package at its installed version.
   Debian packages come from the archive or snapshot.debian.org, Ubuntu
   packages from Launchpad. It also fetches the recorded archives of the
   grafted GCC runtimes and the copyleft Go modules.
4. With `publish` set, it pushes the sources as `<tag>-source` through
   `.github/actions/image-licence-artifacts`, which also attests an SPDX SBOM on
   the published digest. It attaches the notices to the release page and
   writes the release page's licence section.

Without `publish`, the workflow keeps the notices and source lists as a workflow
artifact, for review. Run a step locally:

```bash
python3 scripts/release/published_rc_companion.py export --release v1.0.0-rc.2 --artifact operator --work /tmp/op
python3 scripts/release/published_rc_companion.py licence --release v1.0.0-rc.2 --artifact operator \
  --work /tmp/op --source-tree <checkout of v1.0.0-rc.2>
```

Regenerating a scan builds the tag's configuration and needs its toolchain
(`nvcc` for `cuda`, `icx` / `icpx` for `sycl`, `go` for the Go images):
`python3 scripts/release/published_rc_companion.py scan --release v1.0.0-rc.2 --build cuda`.

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

# GPU variant (CUDA example), and its corresponding source
docker buildx build \
  --platform linux/amd64 \
  --target final-cuda13 \
  -f docker/Dockerfile.production-gpu \
  -t vmafx:test-cuda13 \
  .
docker buildx build --target cuda13-source-export -f docker/Dockerfile.production-gpu \
  --output type=local,dest=./cuda13-source .

# oneAPI variant. Its build looks up the Intel compute-runtime and Level Zero
# releases through the GitHub API; the optional secret lifts the anonymous rate
# limit. `final-oneapi2025` still names the same stage.
GITHUB_TOKEN=$(gh auth token) docker buildx build \
  --platform linux/amd64 \
  --target final-oneapi2026 \
  --secret id=github_token,env=GITHUB_TOKEN \
  -f docker/Dockerfile.production-gpu \
  -t vmafx:test-oneapi2026 \
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
5. **GPU builders and runtimes**: `debian:13-slim` for every builder and
   runtime ([ADR-1517](../adr/1517-gpu-image-licensing.md)); the runtimes carry
   only the vendor files `vmaf` loads.

| Variant | Builder toolchain | Vendor files in the image |
|---------|-------------------|---------------------------|
| CUDA 13.4.2 | `nvcc` from NVIDIA's `debian13` repository at exact versions (`scripts/ci/install-cuda-toolkit.sh --mode=builder`) | None; the host driver provides `libcuda.so.1` |
| ROCm 10.1.0 | `/opt/rocm` streamed out of AMD's pinned `rocm/dev-ubuntu-26.04:10.1.0-full` (`scripts/ci/install-rocm-from-image.sh`) | The HIP runtime files of `tools/rc1-tester/image/hip-runtime.json` |
| Intel oneAPI 2026.1 | oneAPI compiler at one exact apt build ([ADR-1368](../adr/1368-oneapi-release-image-debian13.md)) | The SYCL runtime files of `tools/rc1-tester/image/sycl-runtime.json`, the pinned Intel GPU compute runtime and Level Zero loader |

Every base-image reference is digest-pinned.

### Release publishing and smoke checks

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

See [ADR-0698](../adr/0698-vmafx-production-dockerfile.md) for the full
rationale, alternatives considered, and tag matrix design decisions.

## History

- **GPU images before 1.0.0-rc.3.** Up to v1.0.0-rc.2 the ROCm image was AMD's
  whole `rocm/dev` image (about 29 GB, with compilers, a GPL debugger and a
  profiler library whose licence forbids redistribution), the CUDA image an
  Ubuntu 26.04 base with `libcudart` that nothing loaded, and the oneAPI image
  Intel's full runtime set; none carried notices or published source
  ([Research-2140](../research/2140-production-artifact-licence-audit.md)). Since
  [ADR-1517](../adr/1517-gpu-image-licensing.md) they are Debian 13 images with
  only the vendor files `vmaf` loads.

- **oneAPI compute runtime.** Releases up to v1.0.0-rc.2 shipped the compute
  runtime of Intel's `oneapi-runtime:2025.3.1` image (version 25.18). On an Arc
  B580 it crashed every `--backend sycl` run with a segmentation fault (exit
  code 139) right after device selection, while an Arc A380 and a UHD 770
  worked. The image now carries the same compute runtime as the development
  container.
- **`libumf.so.1` missing in v1.0.0-rc.1.** The `v1.0.0-rc.1` oneAPI image
  passed `vmaf --version` while every Unified Runtime adapter failed to load
  for want of `libumf.so.1`: SYCL reported "No device of requested type
  available" and `--backend sycl` exited with code `100`. The image build and
  the publication smoke test now check the adapters with `ldd`.
