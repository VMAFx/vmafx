# ADR-1053: Default docker-compose runtime to nvidia and expand GPU capabilities

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `dev`, `cuda`, `docker`, `build`

## Context

`dev/docker-compose.yml` previously defaulted `runtime:` to `runc` for both the
`dev-mcp` and `smoke-probe-cron` services, meaning GPU passthrough was only active
when an operator remembered to pass `CONTAINER_RUNTIME=nvidia` explicitly. On any
standard NVIDIA Container Toolkit host the containers silently lost all NVIDIA
access (no `libcuda.so`, no `nvidia-smi`, no NVDEC/NVENC), breaking CUDA scoring,
MCP probes, and the FFmpeg encode paths without any obvious error message.

Additionally, the `deploy.resources.reservations.devices[].capabilities` list was
restricted to `[gpu]`. The NVIDIA Container Toolkit uses this list to decide which
device files and library bind-mounts to inject at runtime. Omitting `compute`,
`utility`, `video`, and `graphics` prevented the container from receiving
`libcuda.so` (compute), `nvidia-smi` / NVML (utility), NVDEC/NVENC (video), and
the Vulkan ICD JSON path needed for CUDA+NVTX interop (graphics — see `x-common-env`
comment in docker-compose.yml).

The `smoke-probe-cron` service also lacked a `deploy` block entirely, so the CDK
never injected any NVIDIA resources there regardless of the runtime setting.

## Decision

Change the default value of `${CONTAINER_RUNTIME}` from `runc` to `nvidia` in both
service definitions. Hosts without the NVIDIA Container Toolkit are instructed to
override via `CONTAINER_RUNTIME=runc docker compose up -d` (documented in an
inline comment). Expand `capabilities` to `[gpu, compute, utility, video, graphics]`
in both services. Add a `deploy` block to `smoke-probe-cron` matching the one in
`dev-mcp`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep `runc` default, document manual override | No change for non-NVIDIA hosts | Silently breaks CUDA on every NVIDIA host unless operator knows the override | Too error-prone; NVIDIA is the primary target |
| Environment-variable-only (no deploy block) | Simpler compose file | `deploy.resources` is required by Docker Swarm and compose v2 for device scheduling; `runtime:` alone does not guarantee NVML injection on newer Toolkit versions | Less portable across compose v2 variants |
| Separate compose override file (`docker-compose.nvidia.yml`) | Clean separation | Adds operational complexity; users must remember to pass `-f docker-compose.nvidia.yml` | Worse UX than a sensible default |

## Consequences

- **Positive**: CUDA scoring, NVDEC/NVENC-accelerated FFmpeg, `nvidia-smi`, and
  NVML all work out-of-the-box on NVIDIA hosts. The smoke probe correctly
  validates the CUDA backend on every 15-minute tick.
- **Negative**: Hosts without the NVIDIA Container Toolkit will see `Error response
  from daemon: unknown or invalid runtime name: nvidia` unless they set
  `CONTAINER_RUNTIME=runc`. This is documented in the inline comment.
- **Neutral / follow-ups**: `dev/Containerfile` and `docs/development/dev-mcp.md`
  may want a note about the override for non-NVIDIA CI environments; left as a
  follow-up item.

## References

- `req` — user direction: wire NVIDIA GPU passthrough in docker-compose.yml; change
  runtime default to nvidia and expand capabilities for both services.
- Related: ADR-0509, ADR-0541 (GPU passthrough rationale), ADR-0528 (/dev/dri
  whole-directory bind-mount).
- NVIDIA Container Toolkit capabilities reference:
  <https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/docker-specialized.html>
