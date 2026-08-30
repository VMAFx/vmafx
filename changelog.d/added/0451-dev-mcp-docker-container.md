- **dev-MCP Docker container with all four GPU backends and continuous smoke probe
  ([ADR-0451](../docs/adr/0451-local-dev-mcp-container.md)).**
  Ships a multi-stage `dev/Containerfile` (Ubuntu 24.04) that installs CUDA 12.6,
  Intel oneAPI 2025.3 (icpx / Level Zero), Vulkan + lavapipe, and ROCm 6.x/HIP,
  then builds libvmaf from source with all backends and the embedded MCP UDS server
  enabled.  `dev/docker-compose.yml` defines two services: `dev-mcp` (primary MCP
  server) and `smoke-probe-cron` (golden-pair probe every 15 minutes across all 4
  backends, writing structured JSON to `.workingdir/dev-mcp-probes/`).  Convenience
  wrappers under `dev/scripts/` cover build, start, stop, shell-attach, and
  single-shot probe.  Operator guide: `docs/development/dev-mcp.md`.
  On NVIDIA-only hosts the HIP toolchain catches compile-time regressions even though
  HIP kernels cannot execute at runtime (`ENOSYS` returned, per ADR-0374 contract).
