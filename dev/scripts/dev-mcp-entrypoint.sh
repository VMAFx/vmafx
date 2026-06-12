#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# dev/scripts/dev-mcp-entrypoint.sh — container entrypoint
#
# Keeps the container alive so MCP clients can attach via
#   docker exec -i vmaf-dev-mcp /opt/vmaf-venv/bin/vmaf-mcp
# (stdio transport — the only one vmaf-mcp's main() implements).
#
# Background: an earlier iteration tried to spawn vmaf-mcp with
# `--transport uds --socket …`, but vmaf-mcp's main() has no argparse
# and ignores all argv flags; it always opens a stdio_server() and
# blocks on stdin. Spawned as a background daemon, its stdin
# immediately closes → the process exits silently → the 30 s socket
# wait timed out → entrypoint exited 1 → docker restarted the
# container in a tight loop. The `.vscode/mcp.json` template and the
# project rule (ADR-0496) already document the docker-exec-i pattern;
# this entrypoint now just keeps the container alive for that pattern
# to work.
#
# If the project later grows a real UDS transport in vmaf-mcp, switch
# this script back to launching it as a daemon and waiting for the
# socket — the previous version of this file is in git.

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
unset VK_ICD_FILENAMES VK_DRIVER_FILES || true

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
mkdir -p /tmp && chmod 1777 /tmp

# ADR-0546: create the vmaf-tune workdir under /probes (the large
# bind-mount). /probes itself is created by the docker-compose bind
# and may not exist yet on a fresh host if the compose file never ran.
# Guard with || true so the entrypoint does not abort on read-only hosts.
# The chown ensures the vmaf user owns the directory even when the host
# bind-mount source is owned by root:root (mode 755), which would otherwise
# cause PermissionError for every bisect worker writing into the workdir.
mkdir -p "${VMAFTUNE_WORKDIR:-/probes/vmaftune-work}" 2>/dev/null || true
chown vmaf:vmaf "${VMAFTUNE_WORKDIR:-/probes/vmaftune-work}" 2>/dev/null || true

LOG_FILE="${VMAF_MCP_LOG:-/tmp/vmaf-mcp.log}"
MODEL_PATH="${VMAF_MODEL_PATH:-/workspace/model}"

# Banner so `docker logs vmaf-dev-mcp` is self-explanatory.
{
  echo "[dev-mcp-entrypoint] vmaf-dev-mcp container ready."
  echo "[dev-mcp-entrypoint] Build info: $(vmaf --version 2>&1 || echo 'vmaf CLI not in PATH')"
  echo "[dev-mcp-entrypoint] Model path: ${MODEL_PATH}"
  echo "[dev-mcp-entrypoint] vmaf-mcp transport: stdio (use 'docker exec -i ${HOSTNAME:-vmaf-dev-mcp} /opt/vmaf-venv/bin/vmaf-mcp')"
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
_probe_with_retry() {
  local label="$1" cmd="$2" pattern="$3" advice="$4"
  local attempt
  for attempt in 1 2 3 4 5 6 7 8 9 10; do
    if eval "${cmd}" 2>&1 | grep -qE "${pattern}"; then
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

{
  echo "[dev-mcp-entrypoint] GPU backend visibility probe (ADR-0540):"
  if command -v sycl-ls >/dev/null 2>&1; then
    _probe_with_retry "SYCL level_zero:gpu" "sycl-ls" "level_zero.*gpu" \
      "Check /dev/dri/renderD<N> passthrough, seccomp=unconfined, and host kernel <-> NEO ABI compat."
  fi
  if command -v rocminfo >/dev/null 2>&1; then
    _probe_with_retry "HIP HSA agent" "rocminfo" "Agent.*GPU|gfx[0-9]+" \
      "Check /dev/kfd passthrough, seccomp=unconfined, and host kernel <-> ROCm KFD ioctl ABI compat."
  fi
} | tee -a "${LOG_FILE}"

# Keep container foreground; Docker's log collector reads stdout.
# We block on `tail -F` so any tool writing to the log shows up in
# `docker logs`. `tail` is shipped with coreutils in the base image.
touch "${LOG_FILE}"
exec tail -F "${LOG_FILE}"
