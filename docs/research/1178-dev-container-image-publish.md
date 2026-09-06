<!-- markdownlint-disable MD013 MD060 -->
# Research Digest: Dev Container Release Artifact Compilation and Runner Architecture

- **Date**: 2026-09-04 (Updated 2026-09-05)
- **Related ADR**: [ADR-1178](../adr/1178-dev-container-image-publish.md)
- **Author**: Lusoris

## 1. Context and Problem Statement

Under [ADR-1102](../adr/1102-phase4b9-container-only-publishing.md), all canonical build artifacts (release binaries, published images, CI artifacts consumed downstream) must be produced inside the `vmaf-dev-mcp` container. However, `.github/workflows/supply-chain.yml` historically built native release artifacts (`libvmaf.so` SONAME chain, `vmaf` CLI binary, `models.tar.gz`) directly on the bare `ubuntu-latest` runner host using ad-hoc `apt-get` and PyPI `meson`.

The container-build detector `scripts/ci/check-container-build.sh` enforces this policy by asserting containerness via `/etc/vmafx-dev-container` and stamping staged artifact trees (`container-build-provenance.txt`).

This research digest evaluates the image dimensions, layer composition, hosted runner disk constraints, and the maintainer decision to execute release artifact compilation on the self-hosted Arc A380 canonical runner.

## 2. Image Metrics and Layer Breakdown

Measured on the workstation on 2026-09-05 with
`docker history --format '{{.Size}}\t{{.CreatedBy}}' vmaf-dev-mcp:local`
(a local build of the full `dev-mcp` target of `dev/Containerfile`; the
`libvmaf-build` stage is a prefix of the same layer stack). The local image ID is `sha256:ff297e6d…`.

| Stage / layer group (`dev/Containerfile`) | Uncompressed size (sum of layers) | Notes |
|---|---|---|
| `build-deps` (`ubuntu:26.04` + apt toolchain) | ~1.6 GB | gcc 15.2, clang 19, meson, ninja, Python 3.14 |
| `gpu-sdks`: CUDA toolkit layer | ~7.7 GB | single `RUN` layer (cuda-toolkit-13-3) |
| `gpu-sdks`: oneAPI + ROCm layer | ~14.2 GB | single `RUN` layer (intel-basekit + ROCm 7.2.4) |
| `gpu-sdks`: Intel NEO / Level Zero / VPL | ~0.5 GB | Level Zero GPU ICD + compute runtime |
| `libvmaf-build`: source copies + venv + ORT + ffmpeg deps | ~5.5 GB | 2.59 GB + 2.16 GB + ~0.5 GB of `COPY` layers |
| **`libvmaf-build` cumulative** | **~29.5 GB** | the stage evaluated for containerised compilation |
| `dev-mcp` additions (ffmpeg, MCP server, Go tools) | ~8 GB | interactive dev tools |
| `dev-mcp` total (`docker history` sum) | ~37.6 GB | `docker images` reports 52.3 GB with containerd snapshotter |

## 3. Runner Disk Footprint — The 29.5 GB Pull Blocker

GitHub's runner reference lists the standard `ubuntu-latest` / `ubuntu-24.04`
runner for **public repositories** as 4 vCPU, 16 GB RAM, **14 GB SSD**
(<https://docs.github.com/en/actions/reference/runners/github-hosted-runners>).

Two physical facts ruled out pulling the image as a job container on `ubuntu-latest`:

1. A ~29.5 GB uncompressed image cannot fit on a 14 GB disk.
2. A job-level `container:` image is pulled by GitHub Actions during
   "Initialize containers", **before any workflow step runs**. Consequently, a disk-clearing step (such as removing Android SDKs or .NET runtimes) cannot precede the pull.

## 4. Evaluation of Build Execution Strategies

| Strategy | Feasibility | Rationale |
|---|---|---|
| **GHCR job-container pull on `ubuntu-latest`** | Infeasible | 29.5 GB uncompressed layer size causes immediate disk exhaustion during image pull. |
| **Slim release-only container stage** | Suboptimal | Violates ADR-1102/ADR-0496 single-container invariant; creates toolchain divergence risk between local development and release outputs. |
| **On-the-fly container build in CI** | Infeasible | Adds 35-45 minutes to every release; risks network timeouts on multi-gigabyte SDK downloads and runner disk exhaustion during intermediate BuildKit caching. |
| **Self-hosted Arc A380 canonical runner (Selected)** | Fully feasible | The runner container is already locally present on the maintainer workstation; zero network pull; exact bit-identical toolchain; enforces container provenance end-to-end. |

## 5. Runner Environment Audit & Gaps Analysis

The self-hosted runner is provisioned by [ADR-1177](../adr/1177-sycl-arc-self-hosted-runner.md) (PR #1304) using `dev/Containerfile.runner` and `dev/docker-compose.runner.yml`.

### Audit of `dev/Containerfile.runner`

- **Base Image**: `ARG BASE_IMAGE=vmaf-dev-mcp:local` -> `FROM ${BASE_IMAGE}`.
- **Inherited Layers**: `vmaf-dev-mcp:local` derives from `dev-mcp` -> `libvmaf-build` -> `gpu-sdks` -> `build-deps`.
- **Toolchains Present**:
  - GCC 13 / GCC 15, Clang 19, LLVM runtime
  - Meson 1.x, Ninja, NASM, pkg-config, CMake
  - Python 3.14 + venv + pip
  - CUDA 13.3, ROCm 7.2.4, Intel oneAPI (icx, icpx, Level Zero)
  - `/etc/vmafx-dev-container` (baked into `build-deps` stage with `vmafx_dev_container=1`)
- **Isolation**: Ephemeral runner running as unprivileged `runner` user; only the Arc A380 DRI node (`/dev/dri/renderD129`) is exposed; no Docker socket mounted; no host filesystem bind mounts.

### Release Build Requirements vs Runner Environment

The native release compilation in `supply-chain.yml` executes:
`meson setup build core --buildtype=release -Denable_avx512=true -Denable_cuda=false -Denable_sycl=false && meson compile -C build`

- **Requirements**: C compiler, Meson, Ninja, NASM, AVX-512 assembler support, coreutils, tar, gzip, sha256sum, git, `/etc/vmafx-dev-container` marker.
- **Runner Environment**: Fully satisfies every requirement natively inside the runner container. No gaps identified.
- **Docker Socket Need**: None. Since the runner container itself IS the build environment, workflow steps execute directly inside the canonical environment without spawning sibling or child containers.

## 6. Decision and Operational Verification

- **Execution**: Release compilation runs on `runs-on: [self-hosted, linux, x64, sycl-arc]` with concurrency group `release-artifacts-build` and `timeout-minutes: 90`.
- **Provenance**: `scripts/ci/check-container-build.sh --stamp artifacts` drops `container-build-provenance.txt`. The gate script was updated to accept both `vmaf-dev-mcp` and `vmaf-sycl-arc-runner` as canonical images while rejecting bare `ubuntu-latest` and unauthorized images.
- **Verification**: `verify-native-artifacts` on `ubuntu-latest` downloads the artifacts and runs `check-container-build.sh --verify artifacts` and `verify-native-release-artifacts.sh`.
- **Optional GHCR Image**: `dev-container-publish.yml` continues to build and publish `ghcr.io/vmafx/vmafx-dev-mcp` on master pushes for external transparency and remote contributors, decoupled from the release path.
