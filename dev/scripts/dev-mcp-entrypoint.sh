#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# dev/scripts/dev-mcp-entrypoint.sh — container entrypoint
#
# Without a command, keeps the container alive so MCP clients can attach via
#   docker exec -i vmaf-dev-mcp vmafx-mcp
# (stdio transport). With a command, runs it and exits with its status.
#
# ADR-1229: the server is the Go binary `vmafx-mcp`, built from cmd/vmafx-mcp
# and already installed at /usr/local/bin by the go-build stage. It replaced
# the Python `mcp-server/vmaf-mcp` package, which implemented the same fifteen
# tools with the same required-argument sets.
#
# Background on why this script does not daemonise the server: an earlier
# iteration tried to spawn the Python server with `--transport uds --socket …`,
# but its main() had no argparse and ignored all argv flags; it always opened a
# stdio_server() and blocked on stdin. Spawned as a background daemon its stdin
# immediately closed → the process exited silently → the 30 s socket wait timed
# out → entrypoint exited 1 → docker restarted the container in a tight loop.
# The docker-exec-i pattern (ADR-0496) avoids that entirely, and the Go server
# is attached the same way, so the reasoning still holds.

set -euo pipefail

# ADR-0509: unset VK_ICD_FILENAMES / VK_DRIVER_FILES on container start.
# Docker's ENV directive cannot truly unset an env var — setting it to ""
# in the Containerfile produces an *empty* value, which the Vulkan loader
# treats as "no ICDs configured" and bails with ERROR_INCOMPATIBLE_DRIVER.
# Unset here so the loader falls back to its default search path
# (/etc/vulkan/icd.d/ + /usr/share/vulkan/icd.d/), which picks up:
#   - NVIDIA's nvidia_icd.json (bind-mounted by nvidia-container-runtime
#     when NVIDIA_DRIVER_CAPABILITIES includes `graphics`),
#   - Mesa's intel_icd.json / radeon_icd.json / lvp_icd.json
#     (installed by mesa-vulkan-drivers in the build-deps stage).
# Operators that need to force a single ICD can still set the env var at
# `docker exec` time per-invocation (e.g. `docker exec -e VK_ICD_FILENAMES=…`).
unset VK_ICD_FILENAMES VK_DRIVER_FILES

# ADR-0541: drop the lavapipe software ICD when at least one real GPU ICD
# is registered. The Vulkan loader enumerates every JSON under
# /etc/vulkan/icd.d/ + /usr/share/vulkan/icd.d/ in lexicographic order and
# first device, which on a stock mesa install is `lvp_icd.json` (lvp_ <
# nvidia_ < intel_ < radeon_). The user-facing symptom is a `vmaf --backend
# vulkan` run that lands on lavapipe (software emulation, ~5x slower than
# CPU) instead of the host's real GPU, with no diagnostic to indicate the
# fallback occurred.
#
# Strategy: if any non-lavapipe ICD JSON is visible at entrypoint time,
# rewrite the ICD search list via VK_DRIVER_FILES (preferred over the
# deprecated VK_ICD_FILENAMES) to the colon-separated list of non-lvp
# JSONs only. If no real ICD is present (CPU-only host, no Container
# Toolkit, no mesa-radeon/intel devices), leave the env vars unset and
# let the loader fall back to lavapipe on its own — the container is
# still usable for build-regression catching.
_vk_real_icds=""
for _vk_dir in /etc/vulkan/icd.d /usr/share/vulkan/icd.d; do
  [ -d "${_vk_dir}" ] || continue
  for _vk_json in "${_vk_dir}"/*.json; do
    [ -e "${_vk_json}" ] || continue
    case "$(basename "${_vk_json}")" in
      lvp_* | lavapipe*) continue ;;
    esac
    if [ -n "${_vk_real_icds}" ]; then
      _vk_real_icds="${_vk_real_icds}:${_vk_json}"
    else
      _vk_real_icds="${_vk_json}"
    fi
  done
done
if [ -n "${_vk_real_icds}" ]; then
  export VK_DRIVER_FILES="${_vk_real_icds}"
  echo "[dev-mcp-entrypoint] Vulkan: pinned non-lavapipe ICDs: ${VK_DRIVER_FILES}"
else
  echo "[dev-mcp-entrypoint] Vulkan: no real ICD found; falling back to mesa default search (lavapipe likely)"
fi
unset _vk_real_icds _vk_dir _vk_json

# ADR-0498 follow-up #8 (BBB e2e v2): some container runtimes ship a
# minimal ``/`` filesystem without ``/tmp`` (especially when the image
# is started with a fresh tmpfs overlay). Both the MCP log and the
# bug-cluster repro scripts assume ``/tmp`` exists with 1777
# permissions. Materialise it idempotently here so the entrypoint
# never fails on "No such file or directory: /tmp/vmaf-mcp.log".
# The entrypoint runs as the unprivileged `vmaf` user and the image's /tmp
# is root's, already 1777: change the mode only when it is wrong and the
# directory is ours. uutils coreutils 0.10 (Ubuntu 26.04 base since #1799)
# issues chmod() even for an unchanged mode, so an unconditional
# `chmod 1777 /tmp` fails with EPERM and `set -e` ends the container.
[ -d /tmp ] || mkdir -p /tmp
if [ "$(stat -c %a /tmp)" != 1777 ]; then
  if [ -O /tmp ]; then
    chmod 1777 /tmp
  else
    echo "[dev-mcp-entrypoint] /tmp is mode $(stat -c %a /tmp) and not owned by $(id -un); continuing" >&2
  fi
fi

# ADR-0546: create the vmaf-tune workdir under /probes (the large
# bind-mount). /probes itself is created by the docker-compose bind
# and may not exist yet on a fresh host if the compose file never ran.
# A failure is logged, not fatal, so the entrypoint does not abort on read-only hosts.
# The chown ensures the vmaf user owns the directory even when the host
# bind-mount source is owned by root:root (mode 755), which would otherwise
# cause PermissionError for every bisect worker writing into the workdir.
if ! mkdir -p "${VMAFTUNE_WORKDIR:-/probes/vmaftune-work}" 2>/dev/null; then
  echo "[dev-mcp-entrypoint] cannot create ${VMAFTUNE_WORKDIR:-/probes/vmaftune-work}; continuing" >&2
fi
if ! chown vmaf:vmaf "${VMAFTUNE_WORKDIR:-/probes/vmaftune-work}" 2>/dev/null; then
  echo "[dev-mcp-entrypoint] cannot chown ${VMAFTUNE_WORKDIR:-/probes/vmaftune-work}; continuing" >&2
fi

# One-shot mode: when the container is started with a command
# (`docker run --rm vmaf-dev-mcp:local pkg-config --modversion vpl`), run it
# and exit with its status. Before this, the arguments were ignored and every
# one-shot probe left a container (and its `vmaf --version` healthcheck) running
# until someone stopped it. The long-running mode below, which keeps the
# container up for `docker exec`, is for a start with no command only.
if [ "$#" -gt 0 ]; then
  exec "$@"
fi

LOG_FILE="${VMAF_MCP_LOG:-/tmp/vmaf-mcp.log}"
MODEL_PATH="${VMAF_MODEL_PATH:-/workspace/model}"

# Banner so `docker logs vmaf-dev-mcp` is self-explanatory.
{
  echo "[dev-mcp-entrypoint] vmaf-dev-mcp container ready."
  echo "[dev-mcp-entrypoint] Build info: $(vmaf --version 2>&1 || echo 'vmaf CLI not in PATH')"
  echo "[dev-mcp-entrypoint] Model path: ${MODEL_PATH}"
  echo "[dev-mcp-entrypoint] vmafx-mcp transport: stdio (use 'docker exec -i ${HOSTNAME:-vmaf-dev-mcp} vmafx-mcp')"
  echo "[dev-mcp-entrypoint] To run vmaf-tune / vmaf-tools inside, e.g.:"
  echo "[dev-mcp-entrypoint]   docker exec ${HOSTNAME:-vmaf-dev-mcp} vmaf --help"
  echo "[dev-mcp-entrypoint]   docker exec ${HOSTNAME:-vmaf-dev-mcp} bash -c 'cd /workspace && PYTHONPATH=tools/vmaf-tune/src python -c \"from vmaftune.cli import main; main()\" --help'"
} | tee -a "${LOG_FILE}"

# ADR-0540: runtime visibility probe for GPU backends that require kernel-UAPI-
# matched userspace (Intel NEO compute-runtime + ROCm). When the host kernel
# is newer than the userspace expects, the userspace silently fails and vmaf
# falls back to CPU. Surface the gap here so operators don't lose hours to
# "why is my Arc / Radeon idle".
#
# The probe retries for up to ~10 seconds: on a freshly-recreated container
# the OCI cgroup device whitelist + seccomp profile are sometimes still
# being applied when the entrypoint starts, so the first sycl-ls / rocminfo
# call after start can race the userspace's first device-open. A short
# retry loop avoids spurious WARN lines on healthy hosts without masking a
# real userspace<->kernel ABI mismatch (which never recovers and keeps
# failing past the retry window).
#
# Security: the probe is ONE program name, run directly as argv[0] with no
# shell interpretation. An earlier form ran `eval "${cmd}"`; with the callers
# below passing constants that was latent, but this script is PID 1 with the
# container's whole environment, so any future caller passing a configured
# value would have been command injection. A probe that needs flags gets its
# own explicit argv handling here — never `eval`, never `bash -c`.
# scripts/ci/tests/test-dev-mcp-entrypoint-probe.sh pins this (pre-commit hook).
_probe_with_retry() {
  local label="$1" prog="$2" pattern="$3" advice="$4"
  local attempt
  for attempt in 1 2 3 4 5 6 7 8 9 10; do
    if "${prog}" 2>&1 | grep -qE "${pattern}"; then
      echo "[dev-mcp-entrypoint]   ${label} detected (attempt ${attempt})"
      return 0
    fi
    sleep 3
  done
  echo "[dev-mcp-entrypoint]   WARN: ${label} NOT detected after 10 attempts (~30 s) — vmaf --backend will fall back to CPU."
  echo "[dev-mcp-entrypoint]         ${advice}"
  # Intentional: return 0 even on miss so `set -e` does not kill the
  # entrypoint and prevent `exec tail -F` from ever running. The WARN
  # itself is the operator-visible signal — the entrypoint must
  # continue so the container stays up for `docker exec` use.
  return 0
}

readonly SYCL_GPU_RECORD_PATTERN='^\[(level_zero|opencl):gpu(:[^]]*)?\]'
readonly HIP_GPU_RECORD_PATTERN='^[[:space:]]*(Name:[[:space:]]+gfx[0-9]+|Device Type:[[:space:]]+GPU)[[:space:]]*$'

{
  echo "[dev-mcp-entrypoint] GPU backend visibility probe (ADR-0540):"
  if command -v sycl-ls >/dev/null 2>&1; then
    _probe_with_retry "SYCL GPU" "sycl-ls" "${SYCL_GPU_RECORD_PATTERN}" \
      "Check /dev/dri/renderD<N> passthrough, seccomp=unconfined, and host kernel <-> NEO ABI compat."
  fi
  if command -v rocminfo >/dev/null 2>&1; then
    _probe_with_retry "HIP HSA GPU agent" "rocminfo" "${HIP_GPU_RECORD_PATTERN}" \
      "Check /dev/kfd passthrough, seccomp=unconfined, and host kernel <-> ROCm KFD ioctl ABI compat."
  fi
} | tee -a "${LOG_FILE}"

# Keep container foreground; Docker's log collector reads stdout.
# We block on `tail -F` so any tool writing to the log shows up in
# `docker logs`. `tail` is shipped with coreutils in the base image.
touch "${LOG_FILE}"
exec tail -F "${LOG_FILE}"
