- **`dev/scripts/dev-mcp-entrypoint.sh`: SYCL and HIP GPU visibility probes
  falsely reported "NOT detected" even when both backends scored correctly at
  invocation time (Bug #2, surfaced by PR #1561 smoke test).** Two pattern
  mismatches: `sycl-ls` emits `[level_zero:gpu:N]` or `[opencl:gpu:N]` lines;
  the old grep `level_zero.*gpu` matched the level_zero form but not the opencl
  fallback.  `rocminfo` emits "Agent N" and "Device Type: GPU" on separate
  lines, so `Agent.*GPU` never matched; the `gfx[0-9]+` alternative worked in
  theory but could miss when rocminfo exited early.  Fixed by anchoring the
  SYCL pattern to `\[(level_zero|opencl):gpu:` and broadening the HIP pattern
  to also match `Device Type: GPU`.  Both probes now log the first output line
  on the initial attempt so operators can distinguish "pattern mismatch" from
  "device truly absent".

- **`dev/scripts/dev-mcp-entrypoint.sh` + `libvmaf/src/libvmaf.c`: Spurious
  "could not open file: /tmp/probe_<B>.json" message on the first vmaf
  invocation in a fresh `docker exec` session (Bug #3, rc=0, file written
  correctly).** The `open(2)` call in `vmaf_write_output_with_format` can
  return `EINTR` on the very first syscall after a new process enters the
  container namespace; the error was printed to stderr and the CLI silently
  swallowed the return value (file written OK on the retry path, but the
  message fired anyway).  Fixed by retrying the `open(2)` call once on `EINTR`
  before printing the error, matching POSIX EINTR-restart convention.

- **`dev/scripts/dev-mcp-entrypoint.sh`: `VK_DRIVER_FILES` set in the
  entrypoint was lost in subsequent `docker exec` sessions (Bug #4).** When
  `exec tail -F` replaces the entrypoint shell, Docker freezes the container's
  base environment (from the Containerfile `ENV` layer + compose
  `environment:`); variables that were dynamically `export`-ed in the
  entrypoint script are not visible to later `docker exec` processes.  Fixed
  by writing `VK_DRIVER_FILES` to `/etc/environment` (read by PAM login
  sessions) and to `/etc/profile.d/vmaf-vk.sh` (sourced by bash login shells,
  which is the pattern documented in the dev-mcp operator guide).

- **`dev/docker-compose.yml`: Transient CUDA `EINVAL` (errno -22) on cold
  start immediately after `docker compose up` (Bug #5).** The healthcheck
  (`vmaf --version`) passes before the NVIDIA Container Toolkit OCI hook
  finishes installing the CUDA bind-mounts; the container becomes "healthy"
  while the first `vmaf --backend cuda` invocation still races the hook.
  Fixed by updating the healthcheck to also verify that `/dev/nvidia0` is a
  character device when it is present (non-NVIDIA hosts skip this check via
  `|| true` short-circuit) and raising `start_period` from 20 s to 45 s to
  accommodate slower host CUDA-init sequences.
