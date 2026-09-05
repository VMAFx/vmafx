#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright 2026 Lusoris
#
# scripts/ci/tests/test-runner-supervisor.sh — Test suite for dev/scripts/runner-supervisor.sh
#
# Exercises:
#   1. Token failure → exponential backoff logged and no container launched.
#   2. Pause file present → supervisor pauses, no compose up executed.
#   3. One successful cycle → acquires token, resolves node, executes compose up.
#   4. SIGTERM clean exit → triggers docker compose down and exits 0.
#
# Exit 0 on all pass, 1 on failure.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../../.." && pwd)"
SUPERVISOR="${REPO_ROOT}/dev/scripts/runner-supervisor.sh"

if [[ ! -x "${SUPERVISOR}" ]]; then
  echo "ERROR: Supervisor script not executable or not found at ${SUPERVISOR}" >&2
  exit 1
fi

TMPDIR_TESTS="$(mktemp -d)"
cleanup() {
  rm -rf -- "${TMPDIR_TESTS}"
}
trap cleanup EXIT

export TMPDIR_TESTS
BIN_DIR="${TMPDIR_TESTS}/bin"
mkdir -p "${BIN_DIR}"

# Stub 'gh'
cat <<EOF >"${BIN_DIR}/gh"
#!/usr/bin/env bash
echo "\$@" >> "${TMPDIR_TESTS}/gh.calls"
if [[ "\${STUB_GH_FAIL:-0}" == "1" ]]; then
  exit 1
fi
if [[ "\$*" =~ registration-token ]]; then
  echo "mock-runner-token-12345"
  exit 0
fi
echo "{}"
EOF
chmod +x "${BIN_DIR}/gh"

# Stub 'docker'
cat <<EOF >"${BIN_DIR}/docker"
#!/usr/bin/env bash
echo "\$@" >> "${TMPDIR_TESTS}/docker.calls"
if [[ "\$1" == "ps" ]]; then
  if [[ -f "${TMPDIR_TESTS}/container_running" ]]; then
    echo "vmaf-sycl-arc-runner"
  fi
  exit 0
fi
if [[ "\$1" == "wait" ]]; then
  echo "wait \$2" >> "${TMPDIR_TESTS}/docker.events"
  rm -f "${TMPDIR_TESTS}/container_running"
  exit 0
fi
if [[ "\$1" == "compose" ]]; then
  if [[ "\$*" =~ "up -d" ]]; then
    echo "COMPOSE_UP: RUNNER_TOKEN=\${RUNNER_TOKEN:-} ARC_RENDER_NODE=\${ARC_RENDER_NODE:-}" >> "${TMPDIR_TESTS}/docker.events"
    touch "${TMPDIR_TESTS}/container_running"
    exit 0
  fi
  if [[ "\$*" =~ "down" ]]; then
    echo "COMPOSE_DOWN" >> "${TMPDIR_TESTS}/docker.events"
    rm -f "${TMPDIR_TESTS}/container_running"
    exit 0
  fi
fi
exit 0
EOF
chmod +x "${BIN_DIR}/docker"

pass=0
fail=0

reset_test_state() {
  rm -f "${TMPDIR_TESTS}/gh.calls" \
    "${TMPDIR_TESTS}/docker.calls" \
    "${TMPDIR_TESTS}/docker.events" \
    "${TMPDIR_TESTS}/container_running" \
    "${TMPDIR_TESTS}/pause" \
    "${TMPDIR_TESTS}/supervisor.log"
}

# -------------------------------------------------------------------------
# Test 1: Token failure -> exponential backoff logged, no compose up
# -------------------------------------------------------------------------
reset_test_state
t1_rc=0
PATH="${BIN_DIR}:${PATH}" \
  STUB_GH_FAIL=1 \
  RUNNER_SUPERVISOR_LOG="${TMPDIR_TESTS}/supervisor.log" \
  RUNNER_PAUSE_FILE="${TMPDIR_TESTS}/pause" \
  ARC_RENDER_NODE="/dev/dri/renderD129" \
  SUPERVISOR_BACKOFF_BASE=1 \
  SUPERVISOR_BACKOFF_MAX=2 \
  SUPERVISOR_MAX_ITERATIONS=2 \
  bash "${SUPERVISOR}" >/dev/null 2>&1 || t1_rc=$?

if [[ "$t1_rc" -eq 0 ]] &&
  grep -q "failed to acquire registration token" "${TMPDIR_TESTS}/supervisor.log" &&
  grep -q "retrying in 1s" "${TMPDIR_TESTS}/supervisor.log" &&
  ! grep -q "COMPOSE_UP" "${TMPDIR_TESTS}/docker.events" 2>/dev/null; then
  echo "PASS test_token_failure_backoff"
  pass=$((pass + 1))
else
  echo "FAIL test_token_failure_backoff (rc=$t1_rc)"
  [[ -f "${TMPDIR_TESTS}/supervisor.log" ]] && sed 's/^/  /' "${TMPDIR_TESTS}/supervisor.log"
  fail=$((fail + 1))
fi

# -------------------------------------------------------------------------
# Test 2: Pause file -> no compose up executed
# -------------------------------------------------------------------------
reset_test_state
touch "${TMPDIR_TESTS}/pause"
t2_rc=0
PATH="${BIN_DIR}:${PATH}" \
  STUB_GH_FAIL=0 \
  RUNNER_SUPERVISOR_LOG="${TMPDIR_TESTS}/supervisor.log" \
  RUNNER_PAUSE_FILE="${TMPDIR_TESTS}/pause" \
  ARC_RENDER_NODE="/dev/dri/renderD129" \
  SUPERVISOR_PAUSE_SLEEP=0 \
  SUPERVISOR_MAX_ITERATIONS=2 \
  bash "${SUPERVISOR}" >/dev/null 2>&1 || t2_rc=$?

if [[ "$t2_rc" -eq 0 ]] &&
  grep -q "paused via" "${TMPDIR_TESTS}/supervisor.log" &&
  ! grep -q "COMPOSE_UP" "${TMPDIR_TESTS}/docker.events" 2>/dev/null; then
  echo "PASS test_pause_file_no_compose_up"
  pass=$((pass + 1))
else
  echo "FAIL test_pause_file_no_compose_up (rc=$t2_rc)"
  [[ -f "${TMPDIR_TESTS}/supervisor.log" ]] && sed 's/^/  /' "${TMPDIR_TESTS}/supervisor.log"
  fail=$((fail + 1))
fi

# -------------------------------------------------------------------------
# Test 3: One successful cycle -> token acquired, compose up called with env
# -------------------------------------------------------------------------
reset_test_state
t3_rc=0
PATH="${BIN_DIR}:${PATH}" \
  STUB_GH_FAIL=0 \
  RUNNER_SUPERVISOR_LOG="${TMPDIR_TESTS}/supervisor.log" \
  RUNNER_PAUSE_FILE="${TMPDIR_TESTS}/pause" \
  ARC_RENDER_NODE="/dev/dri/renderD129" \
  SUPERVISOR_ONCE=1 \
  SUPERVISOR_MAX_ITERATIONS=1 \
  bash "${SUPERVISOR}" >/dev/null 2>&1 || t3_rc=$?

if [[ "$t3_rc" -eq 0 ]] &&
  grep -q "COMPOSE_UP: RUNNER_TOKEN=mock-runner-token-12345 ARC_RENDER_NODE=/dev/dri/renderD129" "${TMPDIR_TESTS}/docker.events" 2>/dev/null &&
  grep -q "runner re-registered (node /dev/dri/renderD129)" "${TMPDIR_TESTS}/supervisor.log"; then
  echo "PASS test_successful_cycle"
  pass=$((pass + 1))
else
  echo "FAIL test_successful_cycle (rc=$t3_rc)"
  [[ -f "${TMPDIR_TESTS}/supervisor.log" ]] && sed 's/^/  /' "${TMPDIR_TESTS}/supervisor.log"
  fail=$((fail + 1))
fi

# -------------------------------------------------------------------------
# Test 4: SIGTERM clean shutdown -> runs docker compose down and exits 0
# -------------------------------------------------------------------------
reset_test_state
PATH="${BIN_DIR}:${PATH}" \
  STUB_GH_FAIL=0 \
  RUNNER_SUPERVISOR_LOG="${TMPDIR_TESTS}/supervisor.log" \
  RUNNER_PAUSE_FILE="${TMPDIR_TESTS}/pause" \
  ARC_RENDER_NODE="/dev/dri/renderD129" \
  SUPERVISOR_CYCLE_SLEEP=1 \
  SUPERVISOR_MAX_ITERATIONS=0 \
  bash "${SUPERVISOR}" >/dev/null 2>&1 &
sub_pid=$!

# Wait briefly for supervisor to start
sleep 0.2
kill -TERM "$sub_pid" 2>/dev/null || true
t4_rc=0
wait "$sub_pid" 2>/dev/null || t4_rc=$?

if [[ "$t4_rc" -eq 0 ]] &&
  grep -q "SIGTERM/SIGINT received" "${TMPDIR_TESTS}/supervisor.log" &&
  grep -q "COMPOSE_DOWN" "${TMPDIR_TESTS}/docker.events" 2>/dev/null; then
  echo "PASS test_sigterm_clean_exit"
  pass=$((pass + 1))
else
  echo "FAIL test_sigterm_clean_exit (rc=$t4_rc)"
  [[ -f "${TMPDIR_TESTS}/supervisor.log" ]] && sed 's/^/  /' "${TMPDIR_TESTS}/supervisor.log"
  fail=$((fail + 1))
fi

echo "Results: ${pass} passed, ${fail} failed"
[[ "$fail" -eq 0 ]]
