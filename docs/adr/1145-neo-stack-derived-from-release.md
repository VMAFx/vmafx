<!-- markdownlint-disable MD013 MD060 -->
# ADR-1145: Derive the Intel NEO compute stack (gmmlib and IGC) dynamically from pinned compute-runtime release metadata

- **Status**: Accepted
- **Date**: 2026-09-02
- **Deciders**: kilian, Antigravity
- **Tags**: `build`, `container`, `supply-chain`, `renovate`, `sycl`, `intel`

## Context

`dev/Containerfile` provisions the Intel GPU compute-runtime stack (NEO) to enable Arc / Xe GPU
dispatch via Level Zero and OpenCL. The stack consists of tightly coupled components that must
function as a matched set: `intel-opencl-icd`, `libze-intel-gpu1` (Level Zero GPU driver),
`libigdgmm12` (Intel Graphics Memory Management), and `intel-igc-core-2` / `intel-igc-opencl-2`
(Intel Graphics Compiler).

In ADR-0605, Renovate regex `customManagers` were introduced to track each component independently:
`ARG NEO_VER`, `ARG GMMLIB_VER`, `ARG IGC_VER`, and `ARG LEVEL_ZERO_VER`. However, `dev/Containerfile`
downloads `libigdgmm12_${GMMLIB_VER}_amd64.deb` directly from the release page of the pinned `NEO_VER`
(`https://github.com/intel/compute-runtime/releases/download/${NEO_VER}/...`).

When upstream `intel/gmmlib` released version `22.10.1` independently of `intel/compute-runtime`,
Renovate opened PR #1184 bumping `GMMLIB_VER=22.10.1` alone while `NEO_VER` remained at `26.31.39395.13`.
This caused a build failure because the pinned NEO release page does not host that gmmlib version.
PR #1188 attempted to mitigate this by configuring a Renovate `packageRule` grouping all four
packages into a single group ("Intel NEO compute stack (matched set)").

However, Renovate grouping can only group updates that are published simultaneously. Because upstream
`intel/gmmlib` releases asynchronously from `intel/compute-runtime`, Renovate opened PR #1205 bumping
only `GMMLIB_VER` to `22.10.1`. The CI check `Dev Container Build + Smoke Test` failed immediately
with exit code 22 (`curl: (22) The requested URL returned error: 404`) trying to fetch:
`https://github.com/intel/compute-runtime/releases/download/26.31.39395.13/libigdgmm12_22.10.1_amd64.deb`.

Tracking `GMMLIB_VER` and `IGC_VER` as independent version pins is fundamentally flawed because their
valid versions and binary package names are strictly defined by the pinned `intel/compute-runtime`
release. The matched set must be *derived*, not tracked independently.

## Decision

We will pin only `ARG NEO_VER` for the Intel compute runtime stack, derive all matching component deb
packages dynamically at build time from the GitHub release metadata of `NEO_VER`, and remove Renovate
managers for `intel/gmmlib` and `intel/intel-graphics-compiler`:

1. **Dynamic Deb Resolution (`dev/scripts/fetch-intel-neo.py`)**:
   - Query `https://api.github.com/repos/intel/compute-runtime/releases/tags/${NEO_VER}`.
   - Dynamically resolve the deb asset URLs for `libigdgmm12_*` (or `intel-igdgmm12_*`),
     `intel-opencl-icd_*`, `libze-intel-gpu1_*` (or `intel-level-zero-gpu_*`), and the `.sum` checksum file.
   - Resolve `intel-igc-core-2_*` and `intel-igc-opencl-2_*` either from release assets if present or
     from the release body installation instructions pointing to the corresponding
     `intel/intel-graphics-compiler` release.
   - Fetch the published sha256 checksums (from `ww*.sum` and the IGC release notes) and verify every
     downloaded `.deb` binary before installation.
   - Log all resolved package names and derived versions (`GMMLIB_VER`, `IGC_VER`) during the build.
   - Fail loudly with a clear diagnostic message if an expected asset is missing, if checksums mismatch,
     or if GitHub API rate limits are hit. Support `ARG GITHUB_TOKEN` for authenticated requests when provided.

2. **Containerfile Clean-up**:
   - Retain `ARG NEO_VER` as the single pin for the compute runtime stack.
   - Retain `ARG LEVEL_ZERO_VER` because the Level Zero loader (`libze1`, `libze-dev`) is published
     independently by `oneapi-src/level-zero`.
   - Remove `ARG GMMLIB_VER` and `ARG IGC_VER`.

3. **Renovate Configuration**:
   - Delete regex `customManagers` for `intel/gmmlib` and `intel/intel-graphics-compiler` in `renovate.json`.
   - Keep `intel/compute-runtime` and `oneapi-src/level-zero`.
   - Update the package group description in `renovate.json` to reflect that gmmlib and IGC are derived at build time.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Derive matched set at build time (chosen) | Eliminates recurring 404 build failures permanently; guarantees bit-exact compatibility verified by upstream checksums; single source of truth (`NEO_VER`). | Requires a Python fetch script and GitHub release API lookup during image build. | **Chosen**. Eliminates the root cause while maintaining strict checksum verification and fail-closed security. |
| Keep Renovate grouping and manually close lone bumps | Zero build script changes. | Fails frequently; breaks CI runs on Renovate PRs; requires continuous human intervention to close bogus PRs like #1184 and #1205. | Rejected. High maintainer friction and recurring broken PR trains. |
| Pin all four and manually bump them | Avoids dynamic network API queries during build. | High maintenance burden; prone to human error when updating `+<build-id>` counters in `IGC_VER`. | Rejected. Upstream NEO releases happen regularly; manual bumping creates unnecessary latency and drift. |
| Vendor the Intel deb packages in the repository | Completely offline builds. | Massive repository bloat (>100MB per release); poor Git ergonomics; violates repository size hygiene. | Rejected. Precompiled vendor binaries do not belong in Git history. |

## Consequences

- **Positive**:
  - Eliminates bogus lone-bump PRs from Renovate for gmmlib and IGC.
  - Bumping `NEO_VER` automatically pulls the exact tested, compatible versions of gmmlib and IGC released by Intel.
  - Every deb package is verified against its published sha256 checksum at build time.
- **Negative**:
  - Image builds now query the GitHub API for `intel/compute-runtime` release metadata. Unauthenticated requests are subject to GitHub API IP rate limits (60 req/hr), though this is mitigated by caching and supporting `GITHUB_TOKEN`.
- **Neutral / follow-ups**:
  - Developer documentation (`docs/development/dev-mcp.md`) and agent instructions (`dev/AGENTS.md`) updated to record that `NEO_VER` is the sole pinned version of the Intel compute runtime stack.

## Supply-chain impact

- **Build-time fetches**: GitHub release API metadata and deb assets for `compute-runtime` and `intel-graphics-compiler`.
- **Integrity verification**: All downloaded binaries are checked against upstream sha256 checksums published in the release (`ww*.sum` and IGC release body).

## References

- PR #1184 (lone gmmlib bump failing with 404)
- PR #1188 (attempted grouping mitigation)
- PR #1205 (recurring lone gmmlib bump failing with 404 at `https://github.com/intel/compute-runtime/releases/download/26.31.39395.13/libigdgmm12_22.10.1_amd64.deb`)
- [ADR-0605](0605-renovate-custommgr-dev-image.md): Renovate customManagers for all dev/Containerfile pinned dependencies
- Maintainer rule: "Intel NEO stack bumps as a matched set"
