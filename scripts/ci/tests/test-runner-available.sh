#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright 2026 Lusoris
#
# scripts/ci/tests/test-runner-available.sh — Test suite for check-runner-available.sh
#
# Tests probe logic across:
#   1. Empty runner list (unregistered)
#   2. Runner registered and online
#   3. Runner registered and offline (must exit 1 with loud error)
#   4. Other runners registered without sycl-arc label
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

run_case() {
  local label="$1"
  local json_payload="$2"
  local want_rc="$3"
  local want_reg="$4"
  local want_avail="$5"

  local out_file="${TMPDIR_TESTS}/output.txt"
  local log_file="${TMPDIR_TESTS}/log.txt"
  : >"$out_file"
  : >"$log_file"

  local rc=0
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

  echo "PASS ${label}"
  pass=$((pass + 1))
}

# Case 1: Empty runner list
JSON_EMPTY='{"total_count":0,"runners":[]}'
run_case "unregistered_empty" "$JSON_EMPTY" 0 "false" "false"

# Case 2: Other runner registered (cuda)
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
run_case "other_label_only" "$JSON_OTHER" 0 "false" "false"

# Case 3: SYCL Arc runner registered and online
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
run_case "sycl_arc_online" "$JSON_ONLINE" 0 "true" "true"

# Case 4: SYCL Arc runner registered and offline (fails loudly)
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
run_case "sycl_arc_offline" "$JSON_OFFLINE" 1 "true" "false"

echo "Results: ${pass} passed, ${fail} failed"
[[ "$fail" -eq 0 ]]
