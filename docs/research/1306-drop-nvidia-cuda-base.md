<!-- markdownlint-disable MD013 -->
# Research: dropping nvidia/cuda base images in favor of version-locked apt installs

**Date**: 2026-09-24
**Question**: Can VMAFx drop all `nvidia/cuda` base image dependencies and replace them with digest-pinned Ubuntu 26.04 plus explicit NVIDIA apt package installation, completely unblocking CUDA bumps from upstream OCI image release lag?
**Answer**: Yes. Verified live against NVIDIA's official package indices and container runtimes. The official apt repository for `ubuntu2604` (`https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2604/x86_64`) serves `cuda-toolkit-13-4` (v13.4.2-1), `cuda-nvcc-13-4` (v13.4.92-1), and `cuda-cudart-13-4` (v13.4.92-1) directly, despite no `nvidia/cuda:13.4.2-*` OCI images existing on Docker Hub.
**Supporting ADR**: [ADR-1306](../adr/1306-drop-nvidia-cuda-base.md).

## 1. Upstream Verification: apt vs OCI publication

When NVIDIA released CUDA 13.4.2, the official redistributable manifest at:
`https://developer.download.nvidia.com/compute/cuda/redist/redistrib_13.4.2.json`
was published immediately (HTTP 200). The apt repository at:
`https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2604/x86_64/`
simultaneously published `cuda-keyring_1.1-1_all.deb`, `cuda-nvcc-13-4` (13.4.92-1), `cuda-cudart-dev-13-4` (13.4.92-1), and `cuda-cudart-13-4` (13.4.92-1).

However, Docker Hub's `nvidia/cuda` repository published zero images for tag `13.4.2-*`. Previously, VMAFx required both `CUDA_BUILDER` and `CUDA_RUNTIME` to pin `nvidia/cuda:<version>-devel-ubuntu26.04@sha256:...` and `nvidia/cuda:<version>-runtime-ubuntu26.04@sha256:...`. Consequently, Renovate and maintainers could not bump CUDA to 13.4.2 without breaking the base image single-source gate (#1525).

## 2. Package Subsets: Builder vs Runtime

The full `cuda-toolkit` meta-package is multi-gigabyte and contains profiling tools, samples, and documentation unneeded for builds. We split installation into two surgical modes via `scripts/ci/install-cuda-toolkit.sh`:

1. **Builder Mode (`--mode=builder`)**:
   - Packages: `cuda-nvcc-${series}` and `cuda-cudart-dev-${series}`.
   - Installs: `/usr/local/cuda-${dotted}/bin/nvcc`, C/C++ development headers, and compiler stubs.
   - Consumed by: `Dockerfile`, `docker/Dockerfile.production-gpu` (`builder-cuda13`), `docker/dev/ubuntu-26.04-cuda.Dockerfile`, and CI runners.
   - Verification: `nvcc --version` executed during install.

2. **Runtime Mode (`--mode=runtime`)**:
   - Packages: `cuda-cudart-${series}`.
   - Installs: `/usr/local/cuda-${dotted}/targets/x86_64-linux/lib/libcudart.so.${major}*` and library symlinks.
   - Excludes compiler, headers, and build tooling, producing minimal distroless/runtime containers.
   - Consumed by: `docker/Dockerfile.production-gpu` (`final-cuda13`) and `docker/Dockerfile.node` (`cuda-runtime-libs`).

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

The remaining coordinated pin inventory consists of 4 sites across 3 files:

1. `build-config.env`: `CUDA_VERSION="13.4.2"` (config)
2. `build-config.env`: `CUDA_APT_PACKAGE="cuda-toolkit-13-4"` (apt)
3. `dev/Containerfile`: `cuda-toolkit-13-4` (apt)
4. `docker/Dockerfile.production-gpu`: `"VMAFX production CUDA 13.4.2 runtime"` (label)

`scripts/ci/check-cuda-pin-lockstep.py` retired the `image` shape from `SITE_SHAPES`. Any re-introduction of an `nvidia/cuda` image tag is caught by `RESIDUAL_RE` as an unrecognized pin, failing closed.
