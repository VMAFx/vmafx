<!-- markdownlint-disable MD013 MD060 -->
# ADR-1272: Align every CUDA consumer on the 13.4 toolchain

- **Status**: Proposed
- **Date**: 2026-09-20
- **Deciders**: Lusoris
- **Tags**: cuda, ci, build, windows, arm64, deps, fork-local

## Context

Renovate PR #1487 moved five NVIDIA container references from CUDA 13.3.1 to
13.4.1 but left `CUDA_VERSION`, the dev-container apt package, both Linux CI
installers, both x64 Windows installers, the Windows ARM64 policy, and user
documentation on 13.3.1. Merging that shape would publish 13.4.1 images while
CI continued compiling with a different toolkit.

The existing installer paths cannot accept a mechanical version substitution.
`Jimver/cuda-toolkit` v0.2.36 has a static download map whose newest entry is
13.3.1. NVIDIA changed the 13.4.1 Windows filenames to carry an explicit
`windows_x86_64` or `windows_arm64` architecture. NVIDIA's Ubuntu 24.04 and
26.04 repositories both carry the `cuda-toolkit-13-4` minor stream, and the
13.4 release publishes native Windows ARM64 `nvcc`, runtime, NVVM, and Visual
Studio integration components. ADR-1260 deliberately deferred its CUDA-on-WoA
compile until this bump.

## Decision

We make `build-config.env` the CUDA authority. `CUDA_VERSION` patch-pins
container and Windows installer artifacts; `CUDA_APT_PACKAGE` pins the matching
Linux minor stream. Linux CI uses NVIDIA's signed apt repository through
`scripts/ci/install-cuda-linux.sh`; Windows CI uses the architecture-qualified
NVIDIA network installer through `scripts/ci/install-cuda-windows.ps1`. The
dev container sources the same config before installing CUDA. The advisory
Windows ARM64 job keeps its CPU fast suite and adds a native build-only CUDA
tree, with architecture-aware MSVC host-compiler selection and PE checks on
both outputs. An executable single-source gate rejects version literals,
consumer-count drift, and a CUDA apt series inconsistent with the patch pin.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep Jimver and CI on 13.3.1 | smallest diff | published images and CI disagree; no Windows ARM64 path; next action release remains external blocker | validation must match what ships |
| Wait for a future Jimver release | retains one third-party action | no release or tag supports 13.4.1 today; merge train remains blocked on an unowned schedule | NVIDIA already publishes authoritative native channels |
| Hard-code 13.4.1 in each workflow | mechanically simple | repeats the exact drift that PR #1487 exposed | `build-config.env` already owns toolchain versions |
| Install the full Linux toolkit in every CI lane | same package as dev container | downloads unused profilers and libraries on hosted runners | CI needs only nvcc and cudart development files |

## Consequences

- **Positive**: images, dev container, Linux CI, x64 Windows CI, and ARM64
  Windows CI share one declared series; 13.4's Windows ARM64 compiler path is
  exercised; removal of Jimver eliminates its release-table lag.
- **Negative**: the repository owns two small installer helpers; the advisory
  ARM64 job downloads CUDA and performs a second compile; NVIDIA apt packages
  follow update releases within 13.4 while container images remain exactly
  13.4.1.
- **Neutral / follow-ups**: a live GPU is still required for CUDA score tests;
  the hosted Windows runners remain compile-only. Promotion of the ARM64 lane
  to required remains a separate maintainer decision under ADR-1260.

## Supply-chain impact

- **Removed dependency**: `Jimver/cuda-toolkit` from the two Linux CUDA
  workflow steps.
- **Build-time fetches**: NVIDIA's `cuda-keyring` package and apt packages on
  Linux; NVIDIA's architecture-qualified network installer and checksum
  manifest on Windows. The Windows helper validates the published checksum and
  Authenticode signer before execution.
- **Trust anchors**: HTTPS plus NVIDIA's apt signing key on Linux; HTTPS,
  NVIDIA's manifest, and the NVIDIA Authenticode certificate on Windows.
- **Runtime surface**: unchanged; the bump replaces CUDA 13.3.1 libraries with
  13.4.1 in the already-existing CUDA image variants.

## References

- [Research digest](../research/1272-cuda-13-4-toolchain-alignment-2026-09-20.md).
- NVIDIA CUDA Toolkit 13.4 release notes and Windows installation guide.
- `Jimver/cuda-toolkit` v0.2.36 `linux-links.ts` and `windows-links.ts`.
- [ADR-0664](0664-windows-cuda-toolkit-installer.md),
  [ADR-1223](1223-cuda-ampere-architecture-floor.md), and
  [ADR-1260](1260-windows-arm64-cpu-lane.md).
- Related PR: #1487.
- Source: `req` — “well but there are version bumps to fix and merge as well no?”
