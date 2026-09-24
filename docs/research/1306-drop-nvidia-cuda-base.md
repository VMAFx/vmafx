<!-- markdownlint-disable MD013 -->
# Research: dropping nvidia/cuda base images in favor of version-locked apt installs

**Date**: 2026-09-24
**Question**: Can VMAFx drop all `nvidia/cuda` base image dependencies and replace them with digest-pinned Ubuntu 26.04 plus explicit NVIDIA apt package installation, completely unblocking CUDA bumps from upstream OCI image release lag?
**Answer**: Yes. Verified live against NVIDIA's official package indices and container runtimes. The official apt repository for `ubuntu2604` (`https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2604/x86_64`) serves `cuda-toolkit-13-4` (v13.4.2-1), `cuda-nvcc-13-4` (v13.4.92-1), `cuda-cudart-dev-13-4` (v13.4.92-1), and `cuda-cudart-13-4` (v13.4.92-1) directly, despite no `nvidia/cuda:13.4.2-*` OCI images existing on Docker Hub.
**Supporting ADR**: [ADR-1306](../adr/1306-drop-nvidia-cuda-base.md).

## 1. Upstream Verification: apt vs OCI publication

When NVIDIA released CUDA 13.4.2, the official redistributable manifest at:
`https://developer.download.nvidia.com/compute/cuda/redist/redistrib_13.4.2.json`
was published immediately (HTTP 200). The apt repository at:
`https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2604/x86_64/`
simultaneously published `cuda-keyring_1.1-1_all.deb`, `cuda-nvcc-13-4` (13.4.92-1), `cuda-cudart-dev-13-4` (13.4.92-1), and `cuda-cudart-13-4` (13.4.92-1).

The package *names* do not lock a point release. The same Packages index still contains older `cuda-nvcc-13-4` 13.4.59-1 and `cuda-cudart[-dev]-13-4` 13.4.49-1 entries alongside 13.4.92-1. Therefore a command that installs only `cuda-*-13-4` can move within the series as repository metadata changes while still claiming CUDA 13.4.2. VMAFx records the exact release-to-component mapping in `build-config.env`, passes `package=version` for every core package, and verifies the installed values with `dpkg-query`.

However, Docker Hub's `nvidia/cuda` repository published zero images for tag `13.4.2-*`. Previously, VMAFx required both `CUDA_BUILDER` and `CUDA_RUNTIME` to pin `nvidia/cuda:<version>-devel-ubuntu26.04@sha256:...` and `nvidia/cuda:<version>-runtime-ubuntu26.04@sha256:...`. Consequently, Renovate and maintainers could not bump CUDA to 13.4.2 without breaking the base image single-source gate (#1525).

## 2. Package Subsets: Builder, Runtime, and Full Development

The full `cuda-toolkit` meta-package is multi-gigabyte and contains profiling tools, samples, and documentation unneeded for ordinary builds. The shared installer therefore exposes three purpose-specific modes via `scripts/ci/install-cuda-toolkit.sh`:

1. **Builder Mode (`--mode=builder`)**:
   - Packages: exact `cuda-nvcc-${series}`, `cuda-cudart-dev-${series}`, and `cuda-cudart-${series}` versions.
   - Installs: `/usr/local/cuda-${dotted}/bin/nvcc`, C/C++ development headers, and compiler stubs.
   - Consumed by: `Dockerfile`, `docker/Dockerfile.production-gpu` (`builder-cuda13`), `docker/dev/ubuntu-26.04-cuda.Dockerfile`, and CI runners.
   - Verification: `nvcc --version` executed during install.

2. **Runtime Mode (`--mode=runtime`)**:
   - Packages: the exact `cuda-cudart-${series}` version.
   - Installs: `/usr/local/cuda-${dotted}/targets/x86_64-linux/lib/libcudart.so.${major}*` and library symlinks.
   - Excludes compiler, headers, and build tooling, producing minimal distroless/runtime containers.
   - Consumed by: `docker/Dockerfile.production-gpu` (`final-cuda13`) and `docker/Dockerfile.node` (`cuda-runtime-libs`).

3. **Full Development Mode (`--mode=full`)**:
   - Packages: exact `cuda-toolkit-${series}`, nvcc, cudart-dev, and cudart versions.
   - Retains the complete development toolkit expected by the all-backend dev-MCP image without maintaining a second apt bootstrap recipe.
   - Consumed by: `dev/Containerfile`.

## 3. Container Execution and Privilege Handling

CI runners execute with `sudo` capability, whereas Docker container build stages run natively as `root` (UID 0) without `sudo` installed. Attempting `sudo apt-get` in a root container fails with `sudo: command not found`.

`scripts/ci/install-cuda-toolkit.sh` handles privilege detection dynamically:

```bash
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  if command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
  else
    echo "::error::install-cuda-toolkit: non-root execution requires sudo" >&2
    exit 1
  fi
fi
```

This guarantees identical execution behavior across root container stages and non-root host/CI workers.

## 4. Impact on Coordinated Pin Inventory

Dropping `nvidia/cuda` base images eliminates 6 `image` sites from the coordinated pin:

- `build-config.env` (`CUDA_BUILDER`, `CUDA_RUNTIME`)
- `Dockerfile` (`ARG CUDA_BUILDER`)
- `docker/Dockerfile.production-gpu` (`ARG CUDA_BUILDER`, `ARG CUDA_RUNTIME`)
- `docker/Dockerfile.node` (`ARG CUDA_RUNTIME`)

The remaining coordinated pin inventory consists of 7 sites across 2 files:

1. `build-config.env`: `CUDA_VERSION="13.4.2"` (config)
2. `build-config.env`: `CUDA_APT_PACKAGE="cuda-toolkit-13-4"` (apt)
3. `build-config.env`: `CUDA_APT_LOCK_RELEASE="13.4.2"` (review latch)
4. `build-config.env`: `CUDA_APT_TOOLKIT_VERSION="13.4.2-1"` (exact apt metadata)
5. `build-config.env`: `CUDA_APT_NVCC_VERSION="13.4.92-1"` (exact apt metadata)
6. `build-config.env`: `CUDA_APT_CUDART_VERSION="13.4.92-1"` (exact apt metadata)
7. `docker/Dockerfile.production-gpu`: `"VMAFX production CUDA 13.4.2 runtime"` (label)

`scripts/ci/check-cuda-pin-lockstep.py` retired the `image` shape from `SITE_SHAPES`. Any re-introduction of an `nvidia/cuda` image tag is caught by `RESIDUAL_RE` as an unrecognized pin, failing closed. `--write` derives only the apt series and runtime label. It deliberately does not guess component build versions: Renovate changes `CUDA_VERSION`, the unchanged `CUDA_APT_LOCK_RELEASE` makes that PR fail, and a maintainer must verify and record the new live mapping.

## 5. Renovate must follow the release channel, not the retired image channel

Removing the Dockerfiles' vendor images was insufficient while Renovate still resolved `CUDA_VERSION` through `nvidia/cuda` tags: discovery would remain blocked on the channel this change removes. The corrected manager uses a `custom.nvidia-cuda-redist` datasource over NVIDIA's official redist directory index. Renovate's HTML datasource turns each hyperlink into a raw release; `extractVersionTemplate` accepts only `redistrib_X.Y.Z.json` and converts it to `X.Y.Z`. Directory links, schema-v2 manifests, signatures, bare versions, and Docker tags do not match.

The HTML conversion does not produce `releaseTimestamp`. The repository's global `minimumReleaseAge: 3 days` defaults to requiring one, which would leave every valid CUDA update pending permanently. A rule matched only to `custom.nvidia-cuda-redist` / `nvidia-cuda-redist` sets `minimumReleaseAgeBehaviour: timestamp-optional`; it keeps `automerge: false` and the `manual-review` label. This is not a new CUDA group: the obsolete `CUDA release (coordinated pin)` group and all `nvidia/cuda` package matches are deleted.

Validation used the project-pinned Renovate 44.111.4 container. `renovate-config-validator --strict --no-global renovate.json` passed, and a local lookup contacted `developer.download.nvidia.com` once and resolved `currentVersion` and `fixedVersion` to `13.4.2` for `custom.nvidia-cuda-redist`. The committed representative-HTML test selects `13.4.2` while rejecting unrelated links.

## 6. Installer tests cannot mutate the host

The installer exposes two test-only path seams: `VMAFX_CUDA_OS_RELEASE_FILE` and `VMAFX_CUDA_PREFIX`. Production callers leave both unset, preserving `/etc/os-release` and `/usr/local`. The contract suite points both into a fresh temporary directory and executes with a fake `PATH` containing no system executables. It proves root and sudo modes, exact builder/runtime/full package operands, installed-version rejection, both runtime symlinks, malformed configuration, stale metadata locks, missing repositories, and invalid arguments without invoking real `apt-get`, `curl`, `sudo`, or writing below `/usr/local`.

The suite has its own `test-install-cuda-toolkit` pre-commit hook. The required `Pre-Commit` workflow executes all hooks, so deleting the exact operands, installed-version checks, `--mode=full`, or the shared dev-container call fails before merge.

### Live no-GPU replay

The final review ran 54 focused installer, pin-ownership, base-image, and Renovate-pattern tests. It also ran the shared script's real `--mode=full` path as root in a disposable Ubuntu 26.04 dev container. That older local image still carried a pre-branch `ubuntu2404` NVIDIA source, so the replay removed that one stale source inside the disposable container before invoking the new script; the host and image were unchanged. The live `ubuntu2604` run completed, repointed `/usr/local/cuda` to `/usr/local/cuda-13.4`, and reported:

```text
Cuda compilation tools, release 13.4, V13.4.92
install-cuda-toolkit: CUDA 13.4.2 full development toolkit
(toolkit 13.4.2-1, nvcc 13.4.92-1, cudart 13.4.92-1) from ubuntu2604
```

Renovate 44.111.4 strict configuration validation passed. `docker buildx build --check` reported no warnings for the root Dockerfile, the Ubuntu CUDA compatibility Dockerfile, both production GPU/node Dockerfiles, and `dev/Containerfile`. The CUDA/base-image/pre-commit hooks, docs fragment check, ADR link check, state-ledger check, ShellCheck, shfmt, Black, Ruff, and Markdownlint all passed. None of these checks requested a GPU.

## 7. Revision-bound GPU acceptance

The source, policy, Dockerfile parser, and no-GPU container checks can be completed while another workload owns the RTX 4090. The final CUDA runtime acceptance must use an image built from the exact clean revision under review and must verify that provenance before touching the device:

```bash
review_sha="$(git rev-parse HEAD)"
test -z "$(git status --porcelain)"
image="vmafx:cuda-${review_sha}"

docker buildx build --load \
  --label "org.opencontainers.image.revision=${review_sha}" \
  --target final-cuda13 \
  --file docker/Dockerfile.production-gpu \
  --tag "${image}" .

test "$(docker image inspect --format '{{ index .Config.Labels "org.opencontainers.image.revision" }}' "${image}")" = "${review_sha}"

docker run --rm --gpus device=0 \
  -v "$PWD/testdata:/data:ro" \
  "${image}" \
  --reference /data/ref_576x324_48f.yuv \
  --distorted /data/dis_576x324_48f.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --backend cuda --model version=vmaf_v0.6.1 \
  --output /dev/stdout --json --quiet
```

At review time GPU 0 was occupied by an unrelated Ollama workload, so this device-bearing smoke was deliberately not run or claimed. It is the sole remaining hardware evidence for closing the state row; no source or policy check is deferred with it.
