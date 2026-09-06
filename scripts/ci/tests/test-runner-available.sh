#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright 2026 Lusoris
#
# scripts/ci/tests/test-runner-available.sh — Test suite for check-runner-available.sh
#
# Pins the ADR-1177 probe contract:
#   1. Lane disabled (RUNNER_ENABLED unset / not "true"): exit 0, available=false,
#      regardless of the runner list (no API call is made).
#   2. Lane enabled, runner registered and online: exit 0, available=true.
#   3. Lane enabled, runner registered but offline: exit 1 (loud).
#   4. Lane enabled, no runner with the label (only other runners): exit 1 (loud).
#   5. Lane enabled, empty runner list: exit 1 (loud).
#   6. Lane enabled, runner list unreadable (API/token failure): exit 1 (loud).
#
# Exit 0 on all pass, 1 on failure.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROBE="${SCRIPT_DIR}/../check-runner-available.sh"

if [[ ! -x "$PROBE" ]]; then
  echo "ERROR: Probe script not executable or not found at ${PROBE}" >&2
  exit 1
fi

TMPDIR_TESTS="$(mktemp -d)"
cleanup() {
  rm -rf -- "$TMPDIR_TESTS"
}
trap cleanup EXIT

pass=0
fail=0

# run_case LABEL ENABLED JSON WANT_RC WANT_REGISTERED WANT_AVAILABLE [EXTRA_ENV...]
run_case() {
  local label="$1"
  local enabled="$2"
  local json_payload="$3"
  local want_rc="$4"
  local want_reg="$5"
  local want_avail="$6"
  shift 6

  local out_file="${TMPDIR_TESTS}/output.txt"
  local log_file="${TMPDIR_TESTS}/log.txt"
  : >"$out_file"
  : >"$log_file"

  local rc=0
  env "$@" \
    RUNNER_ENABLED="$enabled" \
    RUNNERS_JSON="$json_payload" \
    GITHUB_OUTPUT="$out_file" \
    bash "$PROBE" >"$log_file" 2>&1 || rc=$?

  if [[ "$rc" -ne "$want_rc" ]]; then
    echo "FAIL ${label}: expected exit code ${want_rc}, got ${rc}"
    sed 's/^/  /' "$log_file"
    fail=$((fail + 1))
    return
  fi

  local got_reg got_avail
  got_reg="$(sed -n 's/^registered=//p' "$out_file" | tail -n 1)"
  got_avail="$(sed -n 's/^available=//p' "$out_file" | tail -n 1)"

  if [[ "$got_reg" != "$want_reg" ]]; then
    echo "FAIL ${label}: expected registered=${want_reg}, got ${got_reg}"
    fail=$((fail + 1))
    return
  fi

  if [[ "$got_avail" != "$want_avail" ]]; then
    echo "FAIL ${label}: expected available=${want_avail}, got ${got_avail}"
    fail=$((fail + 1))
    return
  fi

  # A loud failure must carry a ::error:: annotation so it is visible in the
  # Checks UI, and a disabled lane must never claim availability.
  if [[ "$want_rc" -eq 1 ]] && ! grep -q '^::error' "$log_file"; then
    echo "FAIL ${label}: exit 1 without a ::error:: annotation"
    fail=$((fail + 1))
    return
  fi

  echo "PASS ${label}"
  pass=$((pass + 1))
}

JSON_EMPTY='{"total_count":0,"runners":[]}'

JSON_OTHER='{
  "total_count": 1,
  "runners": [
    {
      "id": 1,
      "name": "gpu-runner-cuda",
      "status": "online",
      "labels": [{"name": "self-hosted"}, {"name": "cuda"}]
    }
  ]
}'

JSON_ONLINE='{
  "total_count": 1,
  "runners": [
    {
      "id": 2,
      "name": "cachyos-arc-a380",
      "status": "online",
      "labels": [{"name": "self-hosted"}, {"name": "sycl-arc"}]
    }
  ]
}'

JSON_OFFLINE='{
  "total_count": 1,
  "runners": [
    {
      "id": 3,
      "name": "cachyos-arc-a380",
      "status": "offline",
      "labels": [{"name": "self-hosted"}, {"name": "sycl-arc"}]
    }
  ]
}'

# 1. Lane disabled — always a clean skip, whatever the runner list says.
run_case "disabled_unset" "" "$JSON_ONLINE" 0 "false" "false"
run_case "disabled_false" "false" "$JSON_ONLINE" 0 "false" "false"
run_case "disabled_empty_list" "no" "$JSON_EMPTY" 0 "false" "false"

# 2. Lane enabled, runner online.
run_case "enabled_online" "true" "$JSON_ONLINE" 0 "true" "true"

# 3. Lane enabled, runner registered but offline — loud.
run_case "enabled_offline" "true" "$JSON_OFFLINE" 1 "true" "false"

# 4. Lane enabled, only unrelated runners — loud.
run_case "enabled_other_label_only" "true" "$JSON_OTHER" 1 "false" "false"

# 5. Lane enabled, nothing registered — loud.
run_case "enabled_unregistered" "true" "$JSON_EMPTY" 1 "false" "false"

# 6. Lane enabled, runner list unreadable (stands in for a 403 from the API).
run_case "enabled_api_failure" "true" "" 1 "false" "false" \
  "RUNNERS_FILE=${TMPDIR_TESTS}/does-not-exist.json"

# 7. Lane enabled, malformed payload — loud, never silently "unregistered".
run_case "enabled_malformed_json" "true" "not json" 1 "false" "false"

echo "Results: ${pass} passed, ${fail} failed"
[[ "$fail" -eq 0 ]]
