#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright 2026 Lusoris
#
# scripts/ci/check-runner-available.sh — Probe runner registration and status
#
# ADR-1177. Used in CI (.github/workflows/sycl-parity.yml) to check if a runner
# with label $LABEL (default: sycl-arc) is registered and online.
#
# If RUNNERS_JSON is provided (or path in RUNNERS_FILE), parses that.
# Otherwise calls `gh api repos/${REPO}/actions/runners`.
#
# Emits outputs to $GITHUB_OUTPUT if set:
#   registered=true|false
#   available=true|false
#
# Exit code:
#   0: Probe succeeded (either registered & online, or not registered).
#   1: Failure: runner is registered but OFFLINE (fails loudly per ADR-1177).

set -euo pipefail

LABEL="${RUNNER_LABEL:-sycl-arc}"
REPO="${GITHUB_REPOSITORY:-VMAFx/vmafx}"

emit_output() {
  local key="$1" val="$2"
  if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    printf '%s=%s\n' "$key" "$val" >>"$GITHUB_OUTPUT"
  fi
  printf '%s=%s\n' "$key" "$val"
}

# 1. Fetch or load JSON payload
if [[ -n "${RUNNERS_FILE:-}" && -f "${RUNNERS_FILE}" ]]; then
  DATA="$(cat "${RUNNERS_FILE}")"
elif [[ -n "${RUNNERS_JSON:-}" ]]; then
  DATA="${RUNNERS_JSON}"
else
  if ! command -v gh >/dev/null 2>&1; then
    echo "ERROR: 'gh' CLI not found and no RUNNERS_FILE or RUNNERS_JSON supplied." >&2
    exit 1
  fi
  DATA="$(gh api "repos/${REPO}/actions/runners" 2>/dev/null || echo '{"total_count":0,"runners":[]}')"
fi

# 2. Filter runners matching LABEL
MATCHING="$(echo "$DATA" | jq --arg label "$LABEL" '[.runners[]? | select(.labels[]?.name == $label)]')"
COUNT="$(echo "$MATCHING" | jq 'length')"

if [[ "$COUNT" -eq 0 ]]; then
  echo "INFO: No self-hosted runner with label '${LABEL}' is registered in ${REPO}."
  emit_output "registered" "false"
  emit_output "available" "false"
  exit 0
fi

# 3. Check status of matching runners
ONLINE_COUNT="$(echo "$MATCHING" | jq '[.[] | select(.status == "online")] | length')"

if [[ "$ONLINE_COUNT" -gt 0 ]]; then
  echo "INFO: Found ${ONLINE_COUNT} online runner(s) with label '${LABEL}'."
  emit_output "registered" "true"
  emit_output "available" "true"
  exit 0
fi

# 4. Registered but OFFLINE -> Fail loudly
echo "::error::Runner with label '${LABEL}' is registered in ${REPO} but OFFLINE!" >&2
echo "ERROR: ${COUNT} runner(s) registered with label '${LABEL}', but 0 are online." >&2
echo "Check the containerised self-hosted runner service on the host workstation." >&2
emit_output "registered" "true"
emit_output "available" "false"
exit 1
