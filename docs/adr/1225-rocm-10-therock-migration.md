<!-- markdownlint-disable MD013 MD060 -->
# ADR-1225: Migrate the HIP backend to ROCm 10.0.0, installed from digest-pinned container images

- **Status**: Accepted
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: hip, rocm, build, ci, container, dependencies, fork-local

## Context

The fork's HIP backend was built and shipped against ROCm 7.2.3 / 7.2.4,
installed everywhere from AMD's apt channel at
`https://repo.radeon.com/rocm/apt/<version>`: `dev/Containerfile`
(`ARG ROCM_VER=7.2.4`), the two CI HIP lanes
(`.github/workflows/build.yml`, `.github/workflows/libvmaf-build-matrix.yml`),
and the published GPU images (`docker/Dockerfile.production-gpu`,
`docker/Dockerfile.node`).

ROCm 10.0.0 is the current release, and the fork is heading into its 1.0.0
release with a ROCm-legacy toolchain underneath the HIP backend. Bumping is
not a version-string edit, because AMD changed how ROCm is *distributed*:
since ROCm 7.14 the SDK is built and released through "TheRock", and the
legacy apt channel has been frozen. Verified 2026-09-07:

- `repo.radeon.com/rocm/apt/latest` resolves to **7.2.4**;
- `repo.radeon.com/rocm/apt/7.14` and `.../10.0.0` both return **404**;
- the `manylinux` channel tops out at `rocm-rel-7.2.4`;
- the TheRock nightly wheel index carries only `7.14.0a2026061x` alphas;
- the PyPI `rocm-sdk-core` distribution is a 0.1.0 placeholder.

The only stable, digest-pinnable ROCm 10.0.0 artifact is the official
container image `rocm/dev-ubuntu-24.04:10.0.0-full`.

Two further ROCm 10 changes affect this fork directly. First, the install
layout moved to `/opt/rocm/core-10.0/` with `/opt/rocm/lib` as a
Debian-alternatives symlink onto it. Second, `libamdhip64.so`'s link closure
widened: it now pulls `librocprofiler-register`, `librocm_kpack`,
`libamd_comgr`, the bundled `libLLVM` / `libclang-cpp`, and a `rocm_sysdeps`
bundle of vendored zlib/zstd/elf/drm/numa — 19 libraries, 234 MB, wired
together by `$ORIGIN`-relative RPATHs.

## Decision

We will move every ROCm consumer to **ROCm 10.0.0**, installed from the
digest-pinned `rocm/dev-ubuntu-24.04:10.0.0-full` image
(`sha256:a90cf047f615abe70fbef83c64def0a2d549ef37a39c8ea545430aba4981b374`)
rather than from apt, and we will drop the `HSA_OVERRIDE_GFX_VERSION=10.3.0`
aliasing that ROCm 6.x/7.x needed for the fork's `gfx1036` dev host.

Three install shapes, one pinned digest:

- **Containers** (`dev/Containerfile`, `docker/Dockerfile.production-gpu`,
  `docker/Dockerfile.node`) copy `/opt/rocm` out of the pinned image in a
  build stage that first prunes the math libraries libvmaf never links.
- **CI runners** use the new `scripts/ci/install-rocm-from-image.sh`, which
  streams each layer blob from the registry straight into `tar`, extracting
  only `/opt/rocm` minus the same prune list. `docker pull` is not viable on
  a hosted runner: the image is 8.2 GB compressed / 29 GB extracted, and the
  HIP leg shares its runner with the CUDA toolkit and oneAPI. Peak disk is
  the ~5.5 GB result, not the 29 GB image.
- **Workstations** keep using their distribution's ROCm (any 7.0+ release
  still builds the fork) or the dev container.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Stay on ROCm 7.2.4 | Zero work; apt install keeps working | Ships 1.0.0 on a distribution channel AMD has frozen, and CI cannot catch use of any ROCm >= 7.14 API (e.g. `hipMemGetDefaultMemPool`) because the compile lane would not have the headers | Rejected — the user's direction was explicitly to reach ROCm 10 before 1.0.0 |
| Wait for an apt / manylinux channel to carry 10.0.0 | No new install machinery | AMD has published nothing on those channels since 7.2.4 and has stated TheRock is the build/release system going forward; the wait is open-ended | Rejected — indefinite block on a release-gating item |
| `docker pull` the image in CI and `docker cp` `/opt/rocm` out | Simplest possible script | 29 GB in `/var/lib/docker` plus a 5.5 GB copy, on a runner that already carries CUDA + oneAPI; overruns the hosted runner's disk | Rejected — the failure is a disk-full mid-job, not a clean error |
| Mirror a pruned ROCm to `ghcr.io/vmafx/rocm-toolchain` and pull that in CI | ~2 GB pull; no Docker Hub rate-limit exposure | Needs a new publishing workflow and a bootstrap run before this PR's own CI can go green | Deferred — worth doing if Docker Hub anonymous rate limits prove flaky in practice; the extractor script is the same either way |
| Keep `HSA_OVERRIDE_GFX_VERSION=10.3.0` | No behavioural change to the dev host | ROCm 10 supports `gfx1036` natively; the override would alias the agent to `gfx1030` while meson compiles `gfx1036` code objects for the arch `rocm_agent_enumerator` reports — a self-inflicted mismatch | Rejected on measurement (see below) |
| Prune nothing; copy `/opt/rocm` whole | No prune list to maintain | 19 GB per image layer, ~15 GB of it hipBLASLt / rocBLAS / MIOpen / Composable Kernel archives that libvmaf never links | Rejected — the prune is mechanical and verified by a hipcc smoke compile |

## Consequences

- **Positive**: the HIP backend is built against a current, supported ROCm;
  `gfx1036` is a first-class target instead of an aliased one; the
  ROCm payload in the dev container drops from 19 GB to 5.5 GB and the
  node-worker runtime image carries a verified-complete 397 MB closure
  instead of two libraries that no longer suffice.
- **Negative**: the CI HIP lane now costs ~8 GB of download per run
  (~3-5 min, comparable to the apt install it replaces) and depends on
  Docker Hub's anonymous pull budget. The prune list is a maintenance
  surface: it must be re-checked on every ROCm bump, gated by the hipcc
  smoke compile in the `rocm-src` stage.
- **Neutral / follow-ups**: `docs/backends/hip/overview.md`,
  `docs/development/dev-mcp.md` and `dev/AGENTS.md` record the new install
  shape and the two new invariants (never restore the apt path; never prune
  `librocprofiler-register`). If Docker Hub rate limits bite, promote the
  deferred GHCR mirror.

## Verification

Measured on the dev host (Linux 7.2.3-1-cachyos, AMD `gfx1036` the only AMD
agent), 2026-09-07:

```text
rocminfo            → Agent 2  Name: gfx1036   Marketing Name: AMD Radeon Graphics
amdgpu-arch         → gfx1036            (native; no HSA_OVERRIDE_GFX_VERSION)
hipcc --offload-arch=gfx1036 → compiled; kernel ran, h[63] = 63
libvmaf build       → 1537/1537 targets
meson test (HIP)    → Ok: 19, Expected Fail: 4, Fail: 0
end-to-end          → hip vmaf = 45.315104 / cpu vmaf = 45.315104
```

The prune list is gated by a hipcc smoke compile inside the `rocm-src` stage
(19 GB → 5.5 GB, `hipcc --offload-arch=gfx1036` still produces a runnable
binary). An earlier, wider prune that also removed `librocprofiler-register`
made every HIP binary fail at load with
`librocprofiler-register.so.0: cannot open shared object file` — which is why
that library is called out as a hard exclusion from the exclusion list.

The `node-rocm` runtime copy set was verified by resolving
`ldd /opt/rocm/lib/libamdhip64.so` inside a `debian:13-slim` stage carrying
only the copied files: 0 unresolved libraries, 397 MB. The pre-existing flat
copy of `libamdhip64.so*` + `libhsa-runtime64.so*` does **not** satisfy ROCm
10's closure and would have produced an image whose every HIP binary died at
load.

## Supply-chain impact

- **New dependencies**: none at the library level. The build-time source of
  ROCm changes from the `repo.radeon.com` apt repository (GPG-key-signed,
  version-path-pinned) to `rocm/dev-ubuntu-24.04@sha256:a90cf047…`
  (digest-pinned, `build`). A digest pin is a strictly stronger guarantee
  than the apt channel's, which floats within a version path.
- **Removed dependencies**: the `repo.radeon.com` apt source, its GPG
  keyring, and the `rocm-pin-600` apt preference file drop out of the
  container and both CI lanes.
- **Build-time fetches**: `scripts/ci/install-rocm-from-image.sh` performs
  anonymous registry auth plus one manifest and N blob `GET`s against
  `registry-1.docker.io`. The manifest is addressed by digest and the script
  refuses a tag-only reference; blob digests come from that manifest.
- **Sigstore-signable**: AMD does not publish cosign signatures for
  `rocm/dev-ubuntu-24.04`. The trust anchor is the image digest recorded in
  this ADR and in each consumer, which is the same anchor the fork already
  uses for `ubuntu`, `nvidia/cuda` and the distroless bases.
- **CVE surface delta**: narrows. The dev container's ROCm payload drops
  from 19 GB to 5.5 GB and the node-worker's from an incomplete 2-library
  copy to a closed 397 MB set; hipBLASLt, rocBLAS, MIOpen, RCCL, rocFFT,
  rocSPARSE, rocSOLVER and the profilers are no longer present in any
  fork-published image.

## Carbon / footprint

- **Image size**: dev-container ROCm payload 19 GB → 5.5 GB (Δ -13.5 GB).
  Source: `du -sh /opt/rocm` before and after the `rocm-src` prune stage.
  `node-rocm` ROCm payload: 2 libraries (broken) → 397 MB (complete).
  Source: `du -sh /opt/rocm` in the copy-set verification stage.
- **Build-time**: unchanged for incremental builds. CI HIP-lane setup is
  ~8 GB download versus the apt install's ~200 MB, offset against the apt
  path being unable to deliver ROCm 10 at all.

## References

- ROCm release notes — <https://rocm.docs.amd.com/en/latest/about/release-notes.html>
- TheRock build/release system — <https://github.com/ROCm/TheRock>
- `rocm/dev-ubuntu-24.04` tags — <https://hub.docker.com/r/rocm/dev-ubuntu-24.04/tags>
- req: the user reported that the fork is on ROCm legacy and directed a bump
  to 10, noting that the whole SDK and its distribution have changed.
- Q1.1 (target release): 10.0.0.
- Q1.2 (scope): migrate every consumer at once, before 1.0.0.
- Q1.3 (`gfx1036`): keep the target if it works — it does, natively, so the
  `HSA_OVERRIDE_GFX_VERSION` alias is dropped rather than kept.
- [ADR-0541](0541-dev-container-kernel-abi-pins.md) — the kernel-ABI pins this revises.
- [ADR-0542](0542-dev-container-full-gpu-plumbing.md) — introduced the `HSA_OVERRIDE_GFX_VERSION` pin this drops.
- [ADR-0546](0546-audit-cleanup-bundle.md) — the `hip_gfx_targets` fallback list widened to include `gfx1036`.
- [ADR-1129](1129-release-container-runtime-alignment.md) — the release-container runtime alignment whose ROCm reference this moves.
