<!-- markdownlint-disable MD013 MD060 -->
# ADR-0451: Local dev-MCP container for live probing

- **Status**: Accepted
- **Date**: 2026-05-15
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `infra`, `docker`, `mcp`, `gpu`, `hip`, `cuda`, `sycl`, `vulkan`, `dev`, `fork-local`

## Context

The VMAF fork now has four GPU backends (CUDA, SYCL, Vulkan, HIP), an
embedded MCP server (ADR-0128 / ADR-0402), and a growing set of Python tools
(`vmaf-mcp`, `vmaf-train`, `vmaf-tune`).  During active development it became
necessary to probe all backends simultaneously without disrupting the CHUG
overnight training job running on the same machine.

The operator's primary workstation has an RTX 4090 (NVIDIA-only).  The HIP
backend is functionally complete at the toolchain level but there is no AMD GPU
on the host.  Build-regression catching for HIP requires the ROCm compiler
toolchain to be available, but runtime kernel execution is not required for
this purpose.

A persistent, always-on environment that:

1. Builds libvmaf with all four backends enabled.
2. Runs the embedded MCP UDS server continuously.
3. Executes a periodic smoke probe (every 15 minutes) across all backends.
4. Writes structured JSON probe records for CI / audit consumption.

…was identified as the correct next step.  The alternative (a bare-metal
install script) was rejected because it would conflict with the running CHUG
job and because the Docker isolation boundary cleanly separates SDK versions
from the host environment.

## Decision

We will ship a multi-stage `dev/Containerfile` (Ubuntu 24.04 base) together
with `dev/docker-compose.yml` and a set of convenience wrapper scripts under
`dev/scripts/`.

The container installs all four GPU SDKs:

- **CUDA 12.6** — `cuda-toolkit-12-6` from the NVIDIA apt repository.
- **Intel oneAPI 2025.3** — `intel-basekit-2025.3` from the Intel apt repository.
- **Vulkan + lavapipe** — `libvulkan-dev`, `mesa-vulkan-drivers` (software ICD
  for correctness testing without GPU passthrough).
- **ROCm 6.x / HIP** — `rocm-hip-sdk` from the AMD apt repository; toolchain
  only on NVIDIA hosts (no kernel execution).

libvmaf is built from the in-tree source with:

```text
-Denable_cuda=true -Denable_sycl=true -Denable_vulkan=enabled
-Denable_hip=true -Denable_hipcc=true -Denable_metal=auto
-Denable_dnn=enabled -Denable_mcp=true -Denable_mcp_stdio=true
-Denable_mcp_uds=true -Denable_mcp_sse=auto
```

The MCP UDS server starts at container boot via
`dev/scripts/dev-mcp-entrypoint.sh`.  The `smoke-probe-cron` service runs
`dev/scripts/smoke-probe-loop.sh` every 15 minutes against the golden pair
`testdata/ref_576x324_48f.yuv` / `testdata/dis_576x324_48f.yuv`, writing
structured JSON to `.workingdir/dev-mcp-probes/`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Bare-metal install script | No container overhead; fastest startup | Conflicts with the running CHUG training job; pollutes host SDK environment; cannot isolate ROCm 6.x from existing CUDA | Rejected — the CHUG/RTX-4090 conflict is a hard constraint |
| No continuous probe; manual-only | Simpler; no cron noise | Regressions can silently accumulate between sessions; operators forget to probe | Rejected — the cron loop is cheap and the JSON records give a time-series audit trail |
| Single GPU backend only (CPU + CUDA) | Faster build; smaller image | HIP and SYCL toolchain regressions go undetected | Rejected — the user explicitly requested all 4 backends including HIP for build-regression catching even when kernel execution is not possible |
| Two containers per SDK to avoid image bloat | Smaller per-service image | Complex compose topology; hard to run cross-backend probes atomically | Rejected — all probes must share one libvmaf build to be comparable |

## Consequences

- **Positive**: All four backends are exercised on every probe cycle, providing
  a 15-minute maximum detection window for regressions.  The JSON probe records
  form a lightweight time-series audit trail without requiring a database.  The
  Docker boundary prevents the container's SDK stack (ROCm 6.x, oneAPI 2025.3)
  from interfering with the host CUDA environment used by the CHUG training job.
- **Negative**: The container image is large (~8–12 GB compressed) due to
  bundling all four GPU SDK layers.  The first build is slow (20–40 min).  HIP
  kernel execution is not available on NVIDIA-only hosts; probe records will
  show `ENOSYS` for HIP feature extractors — this is expected and documented.
- **Neutral / follow-ups**:
  - The `smoke-probe-cron` probe schema (`backend_results`, `mcp_results`) is
    an internal format.  If a dashboard is built on top of it, formalise the
    schema in a follow-up ADR.
  - Metal remains disabled on Linux (`-Denable_metal=auto`).  The Containerfile
    needs no update when Metal kernel work lands (macOS-only path).
  - The NVIDIA Container Toolkit must be installed on the host separately;
    it cannot be installed inside the container image.

## References

- `req` (2026-05-15 user direction): Docker container preferred; all 4 GPU backends including HIP for build-regression catching; continuous smoke probe loop every 15 minutes.
- [ADR-0128](0128-embedded-mcp-in-libvmaf.md) — embedded MCP scaffold
- [ADR-0402](0402-mcp-runtime-v2.md) — MCP runtime v2 UDS transport
- [ADR-0212](0212-hip-backend-scaffold.md) — HIP backend scaffold
- [ADR-0214](0214-gpu-parity-ci-gate.md) — GPU parity CI gate (ULP thresholds)
- [ADR-0374](0374-disabled-build-enosys-contract.md) — ENOSYS contract for missing GPU backends
- `docs/development/dev-mcp.md` — operator guide for this container
