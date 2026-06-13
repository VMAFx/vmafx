### Fixed

- `dev/docker-compose.yml`: default container runtime changed from `runc` to `nvidia`
  for both `dev-mcp` and `smoke-probe-cron` services, enabling CUDA passthrough on
  NVIDIA Container Toolkit hosts without any manual override. Non-NVIDIA hosts can
  restore the previous behaviour with `CONTAINER_RUNTIME=runc`. GPU capability list
  expanded from `[gpu]` to `[gpu, compute, utility, video, graphics]` so `libcuda.so`,
  `nvidia-smi`/NVML, NVDEC/NVENC, and the CUDA+NVTX interop paths are all injected.
  `smoke-probe-cron` also gains its own `deploy.resources.reservations.devices` block.
  (ADR-1053)
