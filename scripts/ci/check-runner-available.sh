#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# scripts/ci/check-runner-available.sh — Probe an operator-switched self-hosted runner
#
# ADR-1177 / ADR-1319. Runs on a hosted runner before a hardware job and
# decides whether that self-hosted job may run. Each lane has an explicit
# operator switch (passed in as $RUNNER_ENABLED) because the workflow token
# cannot see the runner list:
# GET /repos/{owner}/{repo}/actions/runners needs the "Administration (read)"
# repository permission, which the `permissions:` key of GITHUB_TOKEN cannot
# grant. The API call therefore uses $GH_TOKEN = secrets.SYCL_RUNNER_PROBE_TOKEN
# (a fine-grained PAT, Administration: read-only) and only when the lane is enabled.
#
# Inputs (environment):
#   RUNNER_ENABLED     "true" when the operator has enabled the lane
#                      through its repository variable. Anything else = disabled.
#   RUNNER_LABELS      whitespace/comma-separated complete runs-on label set
#   RUNNER_LABEL       legacy single-label fallback (default: sycl-arc)
#   RUNNER_DISPLAY_NAME human-readable job name (default: SYCL Parity (Arc A380))
#   RUNNER_SWITCH_NAME repository-variable name used in diagnostics
#   RUNNER_RUNBOOK     operator documentation path used in diagnostics
#   GITHUB_REPOSITORY  owner/repo (default: VMAFx/vmafx)
#   RUNNERS_JSON       test hook: use this JSON instead of calling the API
#   RUNNERS_FILE       test hook: read the JSON from this file instead
#   GH_TOKEN           token for `gh api` (needs Administration: read)
#
# Outputs ($GITHUB_OUTPUT when set, always echoed):
#   enabled=true|false      the operator switch as seen by the probe
#   registered=true|false   a runner carrying every required label exists
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

LABELS_RAW="${RUNNER_LABELS:-${RUNNER_LABEL:-sycl-arc}}"
LABELS_RAW="${LABELS_RAW//,/ }"
read -r -a REQUIRED_LABELS <<<"$LABELS_RAW"
LABELS_DISPLAY="$(
  IFS=,
  echo "${REQUIRED_LABELS[*]}"
)"
REPO="${GITHUB_REPOSITORY:-VMAFx/vmafx}"
ENABLED="${RUNNER_ENABLED:-false}"
DISPLAY_NAME="${RUNNER_DISPLAY_NAME:-SYCL Parity (Arc A380)}"
SWITCH_NAME="${RUNNER_SWITCH_NAME:-SYCL_ARC_RUNNER_ENABLED}"
RUNBOOK="${RUNNER_RUNBOOK:-docs/development/ci-self-hosted-sycl.md}"

emit_output() {
  local key="$1" val="$2"
  if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    printf '%s=%s\n' "$key" "$val" >>"$GITHUB_OUTPUT"
  fi
  printf '%s=%s\n' "$key" "$val"
}

fail_loud() {
  # $1 = one-line reason. Marks registered/available as given in $2/$3.
  echo "::error title=${DISPLAY_NAME} runner unavailable::$1" >&2
  echo "ERROR: $1" >&2
  echo "Lane is enabled (${SWITCH_NAME}=true); see ${RUNBOOK}." >&2
  emit_output "enabled" "true"
  emit_output "registered" "$2"
  emit_output "available" "$3"
  exit 1
}

# 0. Operator switch
if [[ "$ENABLED" != "true" ]]; then
  echo "INFO: ${SWITCH_NAME} is not 'true' — ${DISPLAY_NAME} is disabled; the hardware job skips before runner dispatch."
  emit_output "enabled" "false"
  emit_output "registered" "false"
  emit_output "available" "false"
  exit 0
fi

if [[ "${#REQUIRED_LABELS[@]}" -eq 0 ]]; then
  fail_loud "the required runner label set is empty" "false" "false"
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

LABELS_JSON="$(printf '%s\n' "${REQUIRED_LABELS[@]}" | jq -R . | jq -s .)"
if ! MATCHING="$(printf '%s' "$DATA" | jq --argjson required "$LABELS_JSON" \
  '[.runners[]? as $runner | $runner | select($required | all(. as $label | any($runner.labels[]?; (.name | ascii_downcase) == ($label | ascii_downcase))))]' 2>&1)"; then
  fail_loud "runner list is not valid JSON (${MATCHING//$'\n'/ })" "false" "false"
fi
COUNT="$(printf '%s' "$MATCHING" | jq 'length')"

# 2. Enabled but nothing registered (the ephemeral container is not up)
if [[ "$COUNT" -eq 0 ]]; then
  fail_loud "no self-hosted runner with every required label '${LABELS_DISPLAY}' is registered in ${REPO} — provision the documented runner or disable the lane" "false" "false"
fi

# 3. Registered: need at least one online
ONLINE_COUNT="$(printf '%s' "$MATCHING" | jq '[.[] | select(.status == "online")] | length')"

if [[ "$ONLINE_COUNT" -gt 0 ]]; then
  echo "INFO: ${ONLINE_COUNT} online runner(s) with every required label '${LABELS_DISPLAY}' in ${REPO}."
  emit_output "enabled" "true"
  emit_output "registered" "true"
  emit_output "available" "true"
  exit 0
fi

# 4. Registered but every runner is offline
fail_loud "${COUNT} runner(s) with every required label '${LABELS_DISPLAY}' registered in ${REPO} but 0 online — check the runner host" "true" "false"
