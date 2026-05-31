<!-- markdownlint-disable MD060 -->
# ADR-0573: Dev-mcp container — ubuntu:26.04 + CUDA 13.2 + hipcc + ocloc

- **Status**: Superseded by [ADR-0738](0738-bump-cuda-133-r610-local.md)
- **Date**: 2026-05-18
- **Deciders**: lusoris
- **Tags**: `build`, `ci`, `cuda`, `container`, `dev`

## Context

The `vmaf-dev-mcp` container image was based on `ubuntu:24.04` (Noble). The
dev host runs CachyOS with Linux 7.0 and glibc 2.43. A prior attempt
(ADR-0549, PR #1330) to bump the base to `ubuntu:26.04` (Resolute Raccoon,
April 2026 LTS) failed because:

1. **glibc 2.43 `rsqrt` conflict**: Ubuntu 26.04 ships glibc 2.43 which
   declares `rsqrt` (and `rsqrtf`) in `<math.h>`. CUDA 13.0 and 13.1 include
   their own `rsqrt` declarations in device headers that produce
   "conflicting types for 'rsqrt'" errors under glibc 2.43. CUDA 13.2
   addresses this conflict.
2. **ROCm apt source mismatch**: The prior agent switched from the AMD apt repo
   to Ubuntu universe packages for ROCm, reducing the ROCm version to 7.1 and
   losing the `noble` channel pinning that ADR-0541 required for KFD ioctl ABI
   compatibility with Linux 7.x.
3. **mesa-va-drivers removal**: `mesa-libgallium` is the replacement on Ubuntu
   26.04 (verified via `apt-cache show mesa-libgallium` — `Provides:` confirms
   `mesa-va-drivers, va-driver`).

Additionally, two device compilers were missing from the prior container:

- **hipcc** (HIP device compiler): required for `-Denable_hipcc=true` HSACO
  builds. The ROCm 7.2.3 apt packages include `hipcc`; it was not explicitly
  installed.
- **ocloc** (Intel offline OpenCL/SYCL kernel compiler): required for
  `-fsycl-targets=spir64_gen` AOT compilation (PR #1340 /
  chore/sycl-icpx-aot-targets-default). The `intel-ocloc` package ships as a
  separate `.deb` in the Intel NEO GitHub release alongside `intel-opencl-icd`.

Ubuntu 26.04 provides Python 3.14, gcc-13, clang-19, glibc 2.43, mesa 26.0.3,
and kernel 7.x UAPI compatibility.

## Decision

We will base `dev/Containerfile` on `ubuntu:26.04` with the following SDK
source decisions:

**CUDA 13.2** — Two-repo strategy:

- Ubuntu 26.04 keyring from the `ubuntu2604` NVIDIA apt repo provides the
  correct GPG key for apt on 26.04. The `ubuntu2604` repo itself only carries
  driver-side packages (cuda-compat-13-2, nvidia-driver\*).
- A second `sources.list` entry pointing at the `ubuntu2404` NVIDIA apt repo
  carries the full CUDA toolkit packages (nvcc, cudart, headers) as
  `cuda-toolkit-13-2` (verified available as 13.2.0-1). NVIDIA packages
  install fine on glibc 2.43; the two-repo arrangement is the supported
  approach for distributions newer than the toolkit release cycle.
- Install `cuda-toolkit-13-2` (versioned meta-package) rather than the
  unversioned `cuda-toolkit` to pin reproducibly and match CI.

**hipcc** — Install explicitly from the ROCm 7.2.3 noble-channel apt repo as
package `hipcc`. The `rocm-hip-runtime-dev` meta-package may pull it in as a
dependency, but an explicit install entry guards against future dependency
changes. Verified present after install via `apt-mark showmanual | grep hipcc`.

**ocloc** — Install `intel-ocloc_${NEO_VER}-0_amd64.deb` from the Intel NEO
GitHub release alongside `intel-opencl-icd`. The `intel-ocloc` package is a
separate `.deb` in the same release tag (verified in the 26.18.38308.1 release
asset list). It provides `/usr/bin/ocloc`, which `icpx -fsycl-targets=spir64_gen`
calls at build time to embed native ISA blobs for Intel Arc / Xe GPUs. Verified
present after install via `apt-mark showmanual | grep intel-ocloc`.

**ROCm 7.2.3** — Retain `repo.radeon.com/rocm/apt/7.2.3` at the `noble`
codename. The `noble` channel is present (HTTP 200 verified 2026-05-18). A
`resolute` channel does not exist yet. Ubuntu 26.04's glibc 2.43 satisfies all
ROCm 7.2.3 runtime dependencies. ADR-0541's KFD ioctl ABI rationale is
unchanged.

**Intel oneAPI** — Retain `apt.repos.intel.com/oneapi` (unversioned). Distro-
agnostic.

**Intel NEO compute-runtime** — Retain GitHub releases pin
(NEO_VER=26.18.38308.1). Distro-agnostic; installed directly via `dpkg`.

**Python** — Replace `python3.12` with `python3.14` (the only Python in Ubuntu
26.04 universe; 3.12 is removed). Widen `requires-python` from `<3.13` to
`<3.15` in `tools/vmaf-tune/pyproject.toml` and `ai/pyproject.toml`. Raise
the `numpy` ceiling in `python/requirements.txt` from `<2.0.0` to `<3.0.0`
and floor from `>=1.26.4` to `>=2.2.0` — NumPy 1.26.x has no Python 3.14
wheels; NumPy >=2.2.0 does. The Python vmaf harness is compatible with NumPy
2.x (pandas 2.x, scipy 1.17+, scikit-learn 1.8+).

**mesa-va-drivers** — Replace with `mesa-libgallium`, which provides the
`mesa-va-drivers` and `va-driver` virtual packages on Ubuntu 26.04 (mesa
26.0.3). Verified: `apt-cache show mesa-libgallium` on `ubuntu:26.04` shows
`Provides: libglapi-mesa, mesa-va-drivers, mesa-vdpau-drivers, va-driver`.

**CI** — Bump the `Jimver/cuda-toolkit` pin from `13.0.0` to `13.2.0` in
`.github/workflows/libvmaf-build-matrix.yml` (Linux and Windows legs). Jimver
v0.2.35 supports 13.2.0 (verified in `src/links/linux-links.ts` and
`src/links/windows-links.ts`).

## Alternatives considered

| Option | Verdict |
|---|---|
| Stay on ubuntu:24.04 | No Python 3.14; glibc 2.38 mismatch with host 7.x; rsqrt lurks for future CUDA bumps. Host is 26.04; container should match. |
| Use CUDA 13.1 on 26.04 | rsqrt conflict still present in 13.1. Does not resolve the root cause. |
| Add `-D__MATH_NO_INLINES` to NVCC flags | Masks the conflict rather than fixing it; invasive to meson.build. CUDA 13.2 is the correct fix. |
| Use Ubuntu universe for ROCm (as PR #1330 did) | Drops to ROCm 7.1; loses noble-channel pinning; KFD ABI regression risk per ADR-0541. |
| Ubuntu 26.04 resolute ROCm channel | Channel does not exist as of 2026-05-18 (HTTP 404 verified). |
| NumPy <2.0 with source build | NumPy 1.26.x has no CPython 3.14 wheels; building from source in the container is expensive. NumPy 2.x is production-stable. |
| intel-ocloc from apt (intel-opencl-icd) | `intel-ocloc` is a separate package not bundled with `intel-opencl-icd`; must be downloaded explicitly from the NEO release. |

## Consequences

- **Positive**:
  - glibc 2.43 rsqrt conflict eliminated at the SDK level (CUDA 13.2).
  - Container base matches the dev host OS, reducing environment divergence.
  - Python 3.14 support across all in-container Python packages.
  - ROCm 7.2.3 retained, maintaining KFD ABI compatibility with Linux 7.x.
  - `hipcc` explicitly installed and verified; HIP HSACO compilation confirmed.
  - `ocloc` installed; SYCL AOT (`-fsycl-targets=spir64_gen`) confirmed usable.
  - `mesa-libgallium` provides VA-API Gallium drivers for Intel and AMD.
  - CI CUDA pin (13.2.0) matches container CUDA, ensuring local/CI parity.

- **Negative**:
  - NumPy floor raised to 2.2.0: code depending on NumPy 1.x private APIs
    will break. The fork's Python harness does not use private APIs.
  - ROCm noble channel may eventually diverge from resolute; a follow-up bump
    is needed when AMD publishes a resolute channel.

- **Neutral / follow-ups**:
  - When AMD publishes a `resolute` channel for ROCm 7.x, update the apt
    `printf` line in the Containerfile from `noble` to `resolute`.
  - Monitor CUDA 13.2 release notes for additional glibc 2.43 caveats.
  - If rsqrt errors recur in a future CUDA bump, the documented fallback is
    `-D__MATH_NO_INLINES` in `core/src/cuda/meson.build` NVCC flags.
  - Close PR #1330 (prior failed attempt: chore/dev-container-ubuntu-26-04).
  - Update `docs/development/dev-mcp.md` with the 26.04 + CUDA 13.2 baseline
    and the numpy 2.x migration note.

## References

- Supersedes the ubuntu:26.04 attempt in PR #1330
  (branch chore/dev-container-ubuntu-26-04; failed on rsqrt + ROCm regression)
- [ADR-0541](0541-dev-container-gpu-sdk-audit.md) — GPU SDK source decisions
- CUDA 13.2 glibc 2.43 compatibility: NVIDIA Developer Blog, CUDA 13.2 release
  notes
- `apt-cache show mesa-libgallium` on `ubuntu:26.04` (verified 2026-05-18):
  confirms `Provides: mesa-va-drivers, va-driver`
- `repo.radeon.com/rocm/apt/7.2.3/dists/noble/Release` — HTTP 200 verified
  2026-05-18
- `repo.radeon.com/rocm/apt/7.2.3/dists/resolute/Release` — HTTP 404 verified
  2026-05-18
- Jimver/cuda-toolkit v0.2.35 `src/links/linux-links.ts` — 13.2.0 confirmed
- Jimver/cuda-toolkit v0.2.35 `src/links/windows-links.ts` — 13.2.0 confirmed
- intel-compute-runtime 26.18.38308.1 release assets — `intel-ocloc` package
  confirmed present (separate from `intel-opencl-icd`)
- PR #1330 (closed: superseded by this PR)
- req: "Restart the Ubuntu 26.04 base-image bump, bundled with CUDA 13.1 →
  13.2 to unblock the glibc 2.43 rsqrt conflict; add hipcc and ocloc device
  compilers."
