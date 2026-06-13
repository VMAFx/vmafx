<!-- markdownlint-disable MD013 MD060 -->
# ADR-0541: Pin dev-MCP container Intel NEO + ROCm runtimes to versions matching the host kernel

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: build, dev, sycl, hip, container, gpu

## Context

The `vmaf-dev-mcp` container ([`dev/Containerfile`](../../dev/Containerfile)) is
the default execution surface for libvmaf + vmaf-tune work (CLAUDE.md §12 r15).
On 2026-05-18 we observed that both `vmaf --backend sycl` and `vmaf --backend hip`
silently fell back to CPU even though:

- `/dev/dri/renderD<N>` for the Intel Arc A380 was passed through to the
  container (ADR-0528 whole-directory bind mount).
- `/dev/kfd` for AMD KFD was passed through (Containerfile `devices:` entry).
- The Level Zero loader successfully `dlopen()`-ed the Intel GPU adapter
  (`libze_intel_gpu.so.1`) and the OpenCL ICD loader successfully attempted
  `libigdrcl.so`.

The actual failure modes:

- **SYCL** — `zeInit()` returned `ZE_RESULT_ERROR_UNINITIALIZED` (`0x78000001`)
  and `clinfo` reported `Number of platforms 0`. The container shipped Intel
  compute-runtime (NEO) `25.18.33578.15` from Intel's
  `https://repositories.intel.com/gpu/ubuntu noble unified` APT repo (the
  newest version that repo carries as of 2026-05-18). The host runs Linux
  kernel `7.0.8-1-cachyos` with Arch's `intel-compute-runtime 26.18.38308.1`,
  which works. NEO 25.18 is older than the i915 / xe UAPI shipped by Linux
  ≥ 7.0; on that kernel it cannot initialise the GPU. There is no version of
  NEO on the Intel APT noble repo that supports kernel 7.x.
- **HIP** — `rocminfo` reported `Unable to open /dev/kfd read-write: Invalid
  argument`. The container shipped ROCm 6.4 from
  `https://repo.radeon.com/rocm/apt/6.4 noble main`. The host runs Arch's
  ROCm 7.2.3. The KFD ioctl ABI revs across ROCm major versions; ROCm 6.4
  userspace running against Linux ≥ 7.0 KFD returns `EINVAL` for the agent-
  enumeration ioctl, so `hsa_init()` fails and the HSA runtime exposes no
  agents.

Additionally, the oneAPI CPU OpenCL driver
(`/opt/intel/oneapi/compiler/latest/lib/libintelocl.so`) dlopen()s
`libtbb.so.12` at platform-enumeration time. The previous LD_LIBRARY_PATH did
not include `/opt/intel/oneapi/tbb/latest/lib`, so the Khronos ocl-icd loader
silently dropped the Intel CPU OpenCL platform, leaving SYCL with no CPU
fallback either.

Result: on every host newer than the container's pinned ROCm/NEO versions,
`--backend sycl` and `--backend hip` give wrong-but-non-failing output
(CPU scores under a GPU-tagged JSON), defeating the cross-backend probe.

## Decision

We pin the dev-MCP container's Intel NEO compute-runtime and ROCm to versions
that match the host kernel's UAPI: **NEO 26.18.38308.1 via GitHub releases**
(the Intel APT repo's newest is too old) and **ROCm 7.2.3 via the existing
AMD APT repo**. We also add `/opt/intel/oneapi/tbb/latest/lib` to
`LD_LIBRARY_PATH` so the Intel CPU OpenCL driver loads. In
`dev/docker-compose.yml` we add `security_opt: seccomp=unconfined` to the
`dev-mcp` and `smoke-probe-cron` services (newer NEO + ROCm use syscalls
Docker's default seccomp filter blocks, surfacing as INSUFFICIENT_PERMISSIONS
/ EINVAL on device open) and re-add `/dev/dri` as a `devices:` entry alongside
the existing whole-directory bind-mount in `volumes:` (the bind-mount carries
udev symlinks but the cgroup-device whitelist is gated by the `devices:` form
— NEO 26.x enforces the cgroup gate even when the device file is mode 0666;
NEO 25.x tolerated this so the gap was hidden until the version bump). The
`dev-mcp-entrypoint.sh` script gains a retrying visibility probe that warns
on container start if SYCL `level_zero:gpu` or HIP HSA agents are missing.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Pin NEO 26.18 via GitHub-release .debs (chosen for Intel) | Matches host; only path that works for kernel ≥ 7.0 | Five .deb files to download per build instead of one APT meta-package; release tag becomes pinned in Containerfile | Chosen — there is no alternative source for NEO 26.x on Ubuntu 24.04 LTS as of 2026-05-18 |
| Keep Intel `noble/unified` APT repo (25.18) | One-line install; APT handles deps | Newest version in that repo as of 2026-05-18 is too old for Linux 7.x; failure is silent | Rejected — defeats container-default rule (CLAUDE.md §12 r15) when host runs current kernel |
| Pull NEO from Intel's `internal` APT channel | Closer to head | Channel requires Intel SSO + is unstable; not redistributable | Rejected — would break unauthenticated `docker compose build` for non-Intel-employee contributors |
| Pin ROCm 7.2.3 via `repo.radeon.com/rocm/apt/7.2.3 noble main` (chosen for AMD) | One-line repo bump; matches host KFD ABI; same release cadence as Arch host | Image size grows ~200 MB vs 6.4 | Chosen — only ROCm 7.x speaks the kernel-7.x KFD ioctls |
| Pin ROCm to `latest` rolling repo | Automatically tracks newest | Non-reproducible builds; broken releases reach us before AMD's CI catches them | Rejected — version-pin matches the project's "every container rebuild is reproducible" invariant (cf. ADR-0509 / ADR-0535) |
| Downgrade host kernel to 6.x to match container 6.4 | No container change | User's daily driver kernel is 7.x for unrelated reasons; can't gate the dev container on host-OS downgrade | Rejected — fixes container, not host workaround per CLAUDE §12 r15 sub-rule 4 |

## Consequences

- **Positive**: `vmaf --backend sycl` and `vmaf --backend hip` now run real
  kernels on Arc A380 + gfx1036 instead of silent CPU fallback. The
  cross-backend probe (`/cross-backend-diff`) regains its ADR-0214 parity gate
  on Intel + AMD. `dev-mcp-entrypoint.sh` surfaces the failure at container
  start so future host-kernel-ahead-of-container mismatches are caught in
  ≤ 30 s instead of in a confusing CPU-score result.
- **Negative**: Container image grows ~200 MB (ROCm 6.4 → 7.2.3 differential).
  NEO 26.18 is pinned by literal version string (`ARG NEO_VER=26.18.38308.1`);
  when a future kernel-UAPI bump needs a newer NEO, a maintainer has to bump
  the ARG manually. Build now downloads five .deb files from
  `github.com/intel/compute-runtime` releases instead of one APT install.
- **Neutral / follow-ups**:
  - When Intel's `noble/unified` APT repo catches up to NEO 26.18+ we may
    switch back to APT for fewer build-time downloads. Track in
    [`docs/state.md`](../state.md).
  - The ARG-based version pins live alongside `LEVEL_ZERO_VER`, `ORT_VERSION`,
    `NV_CODEC_HEADERS_REF`, and `FFMPEG_TAG` — keep this set in sync with the
    project's overall version-pin discipline (ADR-0509).
  - When the next ROCm major lands and Linux N+1 ships, follow the same
    Containerfile ARG bump path (`ROCM_VER`).

## References

- Source: `req` ("Fix SYCL Level-Zero device discovery in the `vmaf-dev-mcp`
  container ... `vmaf --backend sycl` silently fall back to CPU... Also handle
  HIP/`/dev/kfd` ioctl mismatch...").
- Intel compute-runtime 26.18.38308.1 release:
  `https://github.com/intel/compute-runtime/releases/tag/26.18.38308.1`
- Intel graphics-compiler 2.34.4: NEO 26.18 release notes mandate IGC v2.34.4.
- ROCm 7.2.3 release: `https://repo.radeon.com/rocm/apt/7.2.3/dists/noble/`
- Prior ADRs:
  - [ADR-0509](0509-dev-mcp-icd-loader-vk-icd.md) — initial dev-MCP backend
    visibility (Vulkan loader / LD_LIBRARY_PATH discipline).
  - [ADR-0528](0528-dev-mcp-whole-dri-bind-mount.md) — `/dev/dri` whole-
    directory bind-mount for stable Arc device-node passthrough.
  - [ADR-0214](0214-cross-backend-parity-gate.md) — cross-backend parity gate
    that depends on real GPU execution in the dev-MCP container.
  - CLAUDE.md §12 r15 sub-rule 4 — "Don't reinvent host builds" container-
    default rule.
