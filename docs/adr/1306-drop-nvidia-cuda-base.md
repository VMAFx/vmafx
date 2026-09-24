<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1306: Replace nvidia/cuda base images with digest-pinned Ubuntu and version-locked apt installation

- **Status**: Accepted
- **Date**: 2026-09-24
- **Deciders**: Lusoris
- **Tags**: `docker`, `cuda`, `build`, `ci`, `dependencies`, `security`

## Context

Following [ADR-1231](1231-base-image-single-source.md) and [ADR-1285](1285-cuda-coordinated-pin-lockstep.md), container base images and the coordinated CUDA release pin were centralized in `build-config.env`. However, the CUDA build and runtime containers (`Dockerfile`, `docker/Dockerfile.production-gpu`, `docker/Dockerfile.node`, `docker/dev/ubuntu-26.04-cuda.Dockerfile`) still built directly `FROM nvidia/cuda:<version>-devel-ubuntu26.04` and `FROM nvidia/cuda:<version>-runtime-ubuntu26.04`.

This created a severe dependency bottleneck:

1. **Upstream OCI publication lag**: NVIDIA publishes deb packages to its apt repository on day 1 of a release, but its OCI container images on Docker Hub frequently lag by weeks or skip point releases altogether. For example, CUDA 13.4.2 was available immediately in the official `ubuntu2604` apt repository (`cuda-toolkit-13-4` v13.4.2-1, `cuda-nvcc-13-4` v13.4.92-1) and in the official redistributable JSON manifest (`redistrib_13.4.2.json`), but NVIDIA published no `nvidia/cuda:13.4.2-*` container images.
2. **Coupling inversion**: [ADR-1300](1300-cuda-install-from-nvidia-apt.md) freed CI runners from the `Jimver/cuda-toolkit` action by installing directly from NVIDIA's apt repository. Yet container images remained tied to `nvidia/cuda` base image publication, meaning CI could test on newer CUDA releases that Dockerfiles could not yet build against.
3. **Redundant image sites**: The 6 `image` sites across `build-config.env` and the Dockerfile ARG mirrors represented 60% of the active sites in `scripts/ci/check-cuda-pin-lockstep.py`, requiring digest coordination for vendor images whose base OS was already Ubuntu 26.04.

## Decision

We replace all `nvidia/cuda` base images with digest-pinned Ubuntu 26.04 (`ubuntu:26.04@sha256:...`) matching `DEV_BASE` / `DEV_UBUNTU`, installing the version-locked CUDA toolkit explicitly from NVIDIA's apt repository via a shared, hardened script.

1. **Unified Base Image**:
   In `build-config.env`, `CUDA_BUILDER` and `CUDA_RUNTIME` are set to the identical digest-pinned Ubuntu 26.04 base as `DEV_BASE`:
   `ubuntu:26.04@sha256:da6fc2be547864451aa253836dd926da33623312df4a9a243e35dc877c378a78`.
   `scripts/ci/check-base-image-single-source.sh` requires both keys to equal `DEV_BASE` byte-for-byte, including the digest. The narrow `docker/dev/ubuntu-26.04-cuda.Dockerfile` compatibility image joins the generated base-image inventory; the unrelated Alpine, Arch, and Fedora compatibility images remain independent.

2. **Hardened Shared Installer (`scripts/ci/install-cuda-toolkit.sh`)**:
   The existing CI installer script is generalized and hardened to support:
   - Root container environments where `sudo` is absent, detecting `id -u` and omitting `sudo` when executing as root.
   - Non-root CI/host environments where `sudo` is used for privilege escalation.
   - Distinct installation modes:
     - `--mode=builder`: installs exact `cuda-nvcc-${series}`, `cuda-cudart-dev-${series}`, and `cuda-cudart-${series}` versions (compiler + dev headers + runtime).
     - `--mode=runtime`: installs the exact `cuda-cudart-${series}` version (minimal runtime shared libraries).
     - `--mode=full`: installs the exact `cuda-toolkit-${series}` meta-package plus the exact compiler and cudart components needed by the all-backend dev container.
   - Every apt operand uses `package=version`, and every installed package is checked afterward with `dpkg-query`; resolving a different candidate is a hard failure.
   - `CUDA_APT_LOCK_RELEASE`, `CUDA_APT_TOOLKIT_VERSION`, `CUDA_APT_NVCC_VERSION`, and `CUDA_APT_CUDART_VERSION` record the live NVIDIA package mapping in `build-config.env`. The lock deliberately goes stale when Renovate changes `CUDA_VERSION`, forcing manual metadata review before the bump can pass.
   - Automated prerequisite detection (`curl`, `ca-certificates`).
   - Idempotent `/usr/local/cuda` symlink creation pointing to `/usr/local/cuda-${dotted}`.

3. **Container Adoption**:
   - `Dockerfile`: builds `FROM ${CUDA_BUILDER}`, executes `/tmp/install-cuda-toolkit.sh --mode=builder /tmp`.
   - `docker/Dockerfile.production-gpu`: `builder-cuda13` executes `--mode=builder`; `final-cuda13` builds `FROM ${CUDA_RUNTIME}` and executes `--mode=runtime`.
   - `docker/Dockerfile.node`: `cuda-runtime-libs` builds `FROM ${CUDA_RUNTIME}` and executes `--mode=runtime`, providing `/usr/local/cuda/lib64/libcudart.so*` to the node stage.
   - `docker/dev/ubuntu-26.04-cuda.Dockerfile`: builds `FROM ${CUDA_BUILDER}` and executes `--mode=builder`.
   - `dev/Containerfile`: executes the same shared installer in `--mode=full`; it no longer carries a second keyring/bootstrap or a floating `cuda-toolkit-${series}` apt command.

4. **Coordinated Pin Reduction**:
   `scripts/ci/check-cuda-pin-lockstep.py` retires the `image` shape from `SITE_SHAPES`. The coordinated pin is reduced from 10 sites to 7 sites across 2 files:
   - `build-config.env`: `CUDA_VERSION="13.4.2"`
   - `build-config.env`: `CUDA_APT_PACKAGE="cuda-toolkit-13-4"`
   - `build-config.env`: `CUDA_APT_LOCK_RELEASE="13.4.2"`
   - `build-config.env`: exact toolkit, nvcc, and cudart Debian versions
   - `docker/Dockerfile.production-gpu`: `"VMAFX production CUDA 13.4.2 runtime"`
   The residual regex `RESIDUAL_RE` sweeps for any unrecognized CUDA release literal and catches any re-introduced `nvidia/cuda:*` image tags.

5. **Renovate Release Ownership**:
   The old `nvidia/cuda` Docker datasource and `CUDA release (coordinated pin)` group are removed rather than retained as a hidden OCI dependency. A `custom.nvidia-cuda-redist` HTML datasource reads NVIDIA's official redist index, and the CUDA regex manager accepts only `redistrib_X.Y.Z.json` links through `extractVersionTemplate`. The index exposes no release timestamps, so one datasource-scoped package rule sets `minimumReleaseAgeBehaviour` to `timestamp-optional`; CUDA updates remain manual-review and never automerge. Renovate owns only `CUDA_VERSION`; the gate derives the apt series and runtime label, while the exact metadata lock remains human-reviewed authority.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Wait for upstream `nvidia/cuda:13.4.2` OCI images | Zero Dockerfile script modifications | Point releases are frequently skipped or delayed by weeks; stalls security updates | Unacceptable release latency; blocked #1525 |
| Build and host private base images in an external registry | Avoids apt-get in downstream Dockerfiles | Requires extra build pipelines, secrets, registry hosting, and provenance signing | High infrastructure overhead for a standard apt package set |
| Base on digest-pinned Ubuntu + shared in-tree install script | Day-1 availability of NVIDIA releases; hermetic single source; unifies CI and container recipes | Build stages run apt-get during container build | Chosen. BuildKit cache mounts minimize rebuild times; aligns with ADR-1300 |

## Consequences

- **Positive**:
  - Bumping CUDA versions is completely unblocked from third-party OCI image publication.
  - CUDA builders and runtimes share the exact same Ubuntu 26.04 base OS, glibc, and package ecosystem as the rest of the repository.
  - CUDA apt installs are exact package locks rather than floating `13-4` series selections, and installed versions are independently verified.
  - Six fragile image tag and digest mirrors are eliminated from `check-cuda-pin-lockstep.py`.
  - Renovate discovers CUDA from the same official redist publication channel that exists before vendor OCI images, so automated discovery no longer reintroduces the original bottleneck.
  - Minimal runtime container size: `final-cuda13` installs only `cuda-cudart-13-4` without unnecessary build tools.
- **Negative**:
  - Container build stages without BuildKit cache mounts download apt metadata from NVIDIA on first build.
  - Each CUDA release bump requires checking NVIDIA's live redist manifest and Ubuntu package index to refresh component versions; those versions cannot be derived safely from the marketing release.
- **Neutral / follow-ups**:
  - Amends ADR-1231 and ADR-1285.
  - Windows CI continues using `install-cuda-toolkit.ps1`.
  - ROCm and oneAPI retain their respective vendor configurations.

## References

- Issue: [#1525](https://github.com/VMAFx/vmafx/issues/1525)
- [ADR-1231: Container bases and toolchain versions come from one config file](1231-base-image-single-source.md)
- [ADR-1285: CUDA is a coordinated pin — group it in Renovate, gate the rest](1285-cuda-coordinated-pin-lockstep.md)
- [ADR-1300: Install CUDA on every CI leg from NVIDIA's own distribution](1300-cuda-install-from-nvidia-apt.md)
- Research: [docs/research/1306-drop-nvidia-cuda-base.md](../research/1306-drop-nvidia-cuda-base.md)
