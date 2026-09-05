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

Measured on the workstation on 2026-09-05 with
`docker history --format '{{.Size}}\t{{.CreatedBy}}' vmaf-dev-mcp:local`
(a local build of the full `dev-mcp` target of `dev/Containerfile`; the
`libvmaf-build` stage is a prefix of the same layer stack). The image ID of that
local build is `sha256:ff297e6d…`; it is a local image ID, **not** a GHCR
manifest digest, and nothing under `ghcr.io/vmafx/vmafx-dev-mcp` exists yet
(`docker manifest inspect ghcr.io/vmafx/vmafx-dev-mcp:master` → `manifest unknown`).

| Stage / layer group (`dev/Containerfile`) | Uncompressed size (sum of layers) | Notes |
|---|---|---|
| `build-deps` (`ubuntu:26.04` + apt toolchain) | ~1.6 GB | gcc 15.2, clang, meson, ninja, Python |
| `gpu-sdks`: CUDA toolkit layer | ~7.7 GB | single `RUN` layer |
| `gpu-sdks`: oneAPI + ROCm layer | ~14.2 GB | single `RUN` layer |
| `gpu-sdks`: Intel NEO / Level Zero / VPL | ~0.5 GB | |
| `libvmaf-build`: source copies + venv + ORT + ffmpeg deps | ~5.5 GB | 2.59 GB + 2.16 GB + ~0.5 GB of `COPY` layers |
| **`libvmaf-build` cumulative** | **~29.5 GB** | the stage `dev-container-publish.yml` pushes |
| `dev-mcp` additions (ffmpeg, MCP server, Go tools) | ~8 GB | not pushed |
| `dev-mcp` total (`docker history` sum) | ~37.6 GB | `docker images` reports 52.3 GB with the containerd snapshotter |

Compressed transfer size was **not** measured (no registry push was possible
from the branch); a 2.5–4× ratio for SDK-heavy layers puts it in the
8–12 GB range, which is an estimate, not a measurement.

## 3. Runner Disk Footprint — open blocker

GitHub's runner reference lists the standard `ubuntu-latest` / `ubuntu-24.04`
runner for **public repositories** as 4 vCPU, 16 GB RAM, **14 GB SSD**
(<https://docs.github.com/en/actions/reference/runners/github-hosted-runners>,
read 2026-09-05). Two facts follow:

1. A ~29.5 GB uncompressed image does not fit on a 14 GB disk, regardless of
   how much of the pre-installed tool cache is deleted first.
2. A job-level `container:` image is pulled by the runner during
   "Initialize containers", **before any step runs**, so a disk-clearing step
   cannot help this job shape at all. Only a `docker run` inside a step
   (after freeing space) or a smaller image can.

`dev-container-build.yml` already builds `--target libvmaf-build` on
`ubuntu-latest` as a PR gate; whether that build succeeds within the same
disk budget (BuildKit can discard intermediate layers) is a separate question
from pulling and unpacking the finished stage.

**Consequence for D3:** option (a) as implemented is unverified on a real
runner and, on the published numbers, expected to fail at image pull. Before
this PR leaves draft, either (i) a `workflow_dispatch` dry run of
`dev-container-publish.yml` followed by a run of `build-artifacts` proves the
pull fits (larger runner or a much smaller stage), or (ii) D3 flips to option
(c): a dedicated `release-build` stage in `dev/Containerfile` that inherits
`build-deps` but not `gpu-sdks` (the release artifact is CPU-only:
`-Denable_cuda=false -Denable_sycl=false`), which would be ~2 GB and keep the
single-Containerfile invariant of ADR-1102.

## 4. Decision and Operational Verification

- **Chosen Approach**: Option (a) — Publish canonical container image to GHCR via `dev-container-publish.yml` on pushes to `master` affecting `dev/Containerfile` or `dev/scripts/**`, with digest pinning and cosign keyless signing.
- **Verification Gate**: `scripts/ci/check-container-build.sh --stamp artifacts` inside the container build step, verified by `scripts/ci/check-container-build.sh --verify artifacts` in downstream verification and release attachment gates.
