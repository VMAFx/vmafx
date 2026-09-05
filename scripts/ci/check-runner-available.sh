#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright 2026 Lusoris
#
# scripts/ci/check-runner-available.sh — Probe the Arc A380 self-hosted runner
#
# ADR-1177. Runs on a hosted runner at the start of
# .github/workflows/sycl-parity.yml and decides whether the self-hosted
# `SYCL Parity (Arc A380)` job may run. The lane has an explicit operator
# switch — the repository variable SYCL_ARC_RUNNER_ENABLED (passed in as
# $RUNNER_ENABLED) — because the workflow token cannot see the runner list:
# GET /repos/{owner}/{repo}/actions/runners needs the "Administration (read)"
# repository permission, which the `permissions:` key of GITHUB_TOKEN cannot
# grant. The API call therefore uses $GH_TOKEN = secrets.SYCL_RUNNER_PROBE_TOKEN
# (a fine-grained PAT, Administration: read-only) and only when the lane is
# enabled.
#
# Inputs (environment):
#   RUNNER_ENABLED     "true" when the operator has enabled the lane
#                      (vars.SYCL_ARC_RUNNER_ENABLED). Anything else = disabled.
#   RUNNER_LABEL       runner label to look for (default: sycl-arc)
#   GITHUB_REPOSITORY  owner/repo (default: VMAFx/vmafx)
#   RUNNERS_JSON       test hook: use this JSON instead of calling the API
#   RUNNERS_FILE       test hook: read the JSON from this file instead
#   GH_TOKEN           token for `gh api` (needs Administration: read)
#
# Outputs ($GITHUB_OUTPUT when set, always echoed):
#   enabled=true|false      the operator switch as seen by the probe
#   registered=true|false   a runner carrying $RUNNER_LABEL exists
#   available=true|false    at least one such runner is online -> job may run
#
# Exit code:
#   0  lane disabled (available=false, job skips; the aggregator accepts the
#      skip), or lane enabled and an online runner was found (available=true).
#   1  lane enabled but the runner cannot be used: the API query failed
#      (missing/insufficient token), no runner carries the label (the
#      ephemeral container is not registered), or every such runner is
#      offline. Fails loudly: the dependent job is skipped and the
#      Required Checks Aggregator rejects the skip while the lane is enabled.

set -euo pipefail

LABEL="${RUNNER_LABEL:-sycl-arc}"
REPO="${GITHUB_REPOSITORY:-VMAFx/vmafx}"
ENABLED="${RUNNER_ENABLED:-false}"

emit_output() {
  local key="$1" val="$2"
  if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    printf '%s=%s\n' "$key" "$val" >>"$GITHUB_OUTPUT"
  fi
  printf '%s=%s\n' "$key" "$val"
}

fail_loud() {
  # $1 = one-line reason. Marks registered/available as given in $2/$3.
  echo "::error title=SYCL Parity (Arc A380) runner unavailable::$1" >&2
  echo "ERROR: $1" >&2
  echo "Lane is enabled (SYCL_ARC_RUNNER_ENABLED=true); see docs/development/ci-self-hosted-sycl.md §6." >&2
  emit_output "enabled" "true"
  emit_output "registered" "$2"
  emit_output "available" "$3"
  exit 1
}

# 0. Operator switch
if [[ "$ENABLED" != "true" ]]; then
  echo "INFO: SYCL_ARC_RUNNER_ENABLED is not 'true' — the Arc A380 lane is disabled; the parity job skips and the aggregator accepts the skip (ADR-1177)."
  emit_output "enabled" "false"
  emit_output "registered" "false"
  emit_output "available" "false"
  exit 0
fi

# 1. Fetch or load the runner list
if [[ -n "${RUNNERS_FILE:-}" ]]; then
  if [[ ! -r "${RUNNERS_FILE}" ]]; then
    fail_loud "cannot read RUNNERS_FILE='${RUNNERS_FILE}'" "false" "false"
  fi
  DATA="$(cat "${RUNNERS_FILE}")"
elif [[ -n "${RUNNERS_JSON:-}" ]]; then
  DATA="${RUNNERS_JSON}"
else
  if ! command -v gh >/dev/null 2>&1; then
    fail_loud "'gh' CLI not found and no RUNNERS_FILE / RUNNERS_JSON supplied" "false" "false"
  fi
  if ! DATA="$(gh api "repos/${REPO}/actions/runners" 2>&1)"; then
    fail_loud "GET repos/${REPO}/actions/runners failed (${DATA//$'\n'/ }) — the probe token needs the 'Administration: read' repository permission; set the SYCL_RUNNER_PROBE_TOKEN secret" "false" "false"
  fi
fi

if ! MATCHING="$(printf '%s' "$DATA" | jq --arg label "$LABEL" '[.runners[]? | select(.labels[]?.name == $label)]' 2>&1)"; then
  fail_loud "runner list is not valid JSON (${MATCHING//$'\n'/ })" "false" "false"
fi
COUNT="$(printf '%s' "$MATCHING" | jq 'length')"

# 2. Enabled but nothing registered (the ephemeral container is not up)
if [[ "$COUNT" -eq 0 ]]; then
  fail_loud "no self-hosted runner with label '${LABEL}' is registered in ${REPO} — start the ephemeral container (docs/development/ci-self-hosted-sycl.md §3) or disable the lane" "false" "false"
fi

# 3. Registered: need at least one online
ONLINE_COUNT="$(printf '%s' "$MATCHING" | jq '[.[] | select(.status == "online")] | length')"

if [[ "$ONLINE_COUNT" -gt 0 ]]; then
  echo "INFO: ${ONLINE_COUNT} online runner(s) with label '${LABEL}' in ${REPO}."
  emit_output "enabled" "true"
  emit_output "registered" "true"
  emit_output "available" "true"
  exit 0
fi

# 4. Registered but every runner is offline
fail_loud "${COUNT} runner(s) with label '${LABEL}' registered in ${REPO} but 0 online — check the container on the workstation" "true" "false"
