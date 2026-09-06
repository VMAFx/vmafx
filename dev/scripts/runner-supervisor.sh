#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# runner-supervisor.sh — Host supervisor loop for the Intel Arc A380 containerised
# self-hosted runner.
#
# Lifecycle:
#   1. Check pause file (${XDG_STATE_HOME:-$HOME/.local/state}/vmafx-runner/pause).
#      If present, sleep and continue without launching a container.
#   2. If a container named 'vmaf-sycl-arc-runner' is already running, wait for it
#      to complete its ephemeral job via `docker wait`, then tear it down via
#      `docker compose down`.
#   3. Mint a fresh runner registration token via `gh api`.
#   4. Resolve the Arc render node via `dev/scripts/arc-render-node.sh`.
#   5. Start the container via `docker compose up -d`.
#   6. Repeat.
#
# Token authentication:
#   By default, `gh` uses the maintainer user's credentials on the host.
#   For headless servers, provide `GH_TOKEN` or `GITHUB_TOKEN` in the environment
#   (or ~/.config/vmafx-runner/env when running under systemd --user).
#
# Clean termination:
#   On SIGTERM or SIGINT, catches the signal, tears down any running container via
#   `docker compose down`, and exits cleanly (0).
#
# Governing ADR: ADR-1177. Runbook: docs/development/ci-self-hosted-sycl.md.

set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
COMPOSE_FILE="${REPO_ROOT}/dev/docker-compose.runner.yml"
ARC_NODE_SCRIPT="${REPO_ROOT}/dev/scripts/arc-render-node.sh"

REPO="${GITHUB_REPOSITORY:-VMAFx/vmafx}"
CONTAINER_NAME="${RUNNER_CONTAINER_NAME:-vmaf-sycl-arc-runner}"
STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/vmafx-runner"
LOG_FILE="${RUNNER_SUPERVISOR_LOG:-${STATE_DIR}/supervisor.log}"
PAUSE_FILE="${RUNNER_PAUSE_FILE:-${STATE_DIR}/pause}"
PAUSE_SLEEP_SECONDS="${SUPERVISOR_PAUSE_SLEEP:-60}"
CYCLE_SLEEP_SECONDS="${SUPERVISOR_CYCLE_SLEEP:-20}"
BACKOFF_BASE_SECONDS="${SUPERVISOR_BACKOFF_BASE:-10}"
BACKOFF_MAX_SECONDS="${SUPERVISOR_BACKOFF_MAX:-300}"
MAX_ITERATIONS="${SUPERVISOR_MAX_ITERATIONS:-0}"

mkdir -p "$(dirname -- "${LOG_FILE}")"
mkdir -p "$(dirname -- "${PAUSE_FILE}")"

log_msg() {
  local ts
  ts="$(date +'%Y-%m-%d %H:%M:%S')"
  printf '[%s] %s\n' "$ts" "$*" | tee -a "${LOG_FILE}" >&2
}

cleanup() {
  log_msg "SIGTERM/SIGINT received; shutting down runner supervisor"
  docker compose -f "${COMPOSE_FILE}" down >/dev/null 2>&1 || true
  exit 0
}
trap cleanup SIGTERM SIGINT

current_backoff="${BACKOFF_BASE_SECONDS}"
iteration=0

cd "${REPO_ROOT}"

while true; do
  if [[ "${MAX_ITERATIONS}" -gt 0 && "${iteration}" -ge "${MAX_ITERATIONS}" ]]; then
    log_msg "reached max iterations (${MAX_ITERATIONS}); stopping supervisor"
    break
  fi
  iteration=$((iteration + 1))

  # 1. Pause file check
  if [[ -f "${PAUSE_FILE}" ]]; then
    log_msg "paused via ${PAUSE_FILE}; checking again in ${PAUSE_SLEEP_SECONDS}s"
    sleep "${PAUSE_SLEEP_SECONDS}"
    continue
  fi

  # 2. Wait for any active runner container to finish its ephemeral job
  if docker ps --format '{{.Names}}' 2>/dev/null | grep -qx "${CONTAINER_NAME}"; then
    log_msg "runner container '${CONTAINER_NAME}' alive; waiting for job completion"
    docker wait "${CONTAINER_NAME}" >/dev/null 2>&1 || true
    docker compose -f "${COMPOSE_FILE}" down >/dev/null 2>&1 || true
    log_msg "job finished; container removed"
    sleep 5
  fi

  # Ensure no stale stopped container is hanging around
  if docker ps -a --format '{{.Names}}' 2>/dev/null | grep -qx "${CONTAINER_NAME}"; then
    docker compose -f "${COMPOSE_FILE}" down >/dev/null 2>&1 || true
  fi

  # Re-check pause in case it was created while waiting for job completion
  if [[ -f "${PAUSE_FILE}" ]]; then
    log_msg "paused via ${PAUSE_FILE}; checking again in ${PAUSE_SLEEP_SECONDS}s"
    sleep "${PAUSE_SLEEP_SECONDS}"
    continue
  fi

  # 3. Mint fresh registration token
  token=""
  if token_out="$(gh api -X POST "repos/${REPO}/actions/runners/registration-token" --jq .token 2>>"${LOG_FILE}")"; then
    token="$(printf '%s' "${token_out}" | tr -d '[:space:]')"
  fi

  if [[ -z "${token}" ]]; then
    log_msg "failed to acquire registration token from gh; retrying in ${current_backoff}s"
    sleep "${current_backoff}"
    current_backoff=$((current_backoff * 2))
    if [[ "${current_backoff}" -gt "${BACKOFF_MAX_SECONDS}" ]]; then
      current_backoff="${BACKOFF_MAX_SECONDS}"
    fi
    continue
  fi

  # 4. Resolve Intel Arc render node
  render_node="${ARC_RENDER_NODE:-}"
  if [[ -z "${render_node}" ]]; then
    if node_out="$(bash "${ARC_NODE_SCRIPT}" 2>>"${LOG_FILE}")"; then
      render_node="$(printf '%s' "${node_out}" | tr -d '[:space:]')"
    fi
  fi

  if [[ -z "${render_node}" ]]; then
    log_msg "failed to resolve Intel Arc render node; retrying in ${current_backoff}s"
    sleep "${current_backoff}"
    current_backoff=$((current_backoff * 2))
    if [[ "${current_backoff}" -gt "${BACKOFF_MAX_SECONDS}" ]]; then
      current_backoff="${BACKOFF_MAX_SECONDS}"
    fi
    continue
  fi

  # 5. Launch container via docker compose
  export RUNNER_TOKEN="${token}"
  export ARC_RENDER_NODE="${render_node}"
  if ! docker compose -f "${COMPOSE_FILE}" up -d >>"${LOG_FILE}" 2>&1; then
    log_msg "docker compose up failed; retrying in ${current_backoff}s"
    sleep "${current_backoff}"
    current_backoff=$((current_backoff * 2))
    if [[ "${current_backoff}" -gt "${BACKOFF_MAX_SECONDS}" ]]; then
      current_backoff="${BACKOFF_MAX_SECONDS}"
    fi
    continue
  fi

  # 6. Reset backoff on success
  current_backoff="${BACKOFF_BASE_SECONDS}"
  log_msg "runner re-registered (node ${render_node}); listening for jobs"

  # If running in single-cycle mode, exit after starting
  if [[ "${SUPERVISOR_ONCE:-0}" == "1" ]]; then
    break
  fi

  sleep "${CYCLE_SLEEP_SECONDS}"
done
