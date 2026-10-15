#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# scripts/adr/tests/test-next-free-stale-lock.sh
#
# Lock-recovery test for `next-free.sh --claim` (issue #2784). A claim killed
# while it holds the allocator lock leaves the lock directory behind; before
# the fix every later claim waited 10 s and failed with "could not acquire ADR
# allocator lock". Three cases, each in a throw-away repository:
#
#   1. positive: a lock whose owner process is gone is taken over;
#   2. positive: a lock directory without an owner file (a claim killed
#      between mkdir and the owner write) is taken at once;
#   3. negative: a lock whose owner process is alive is respected, and the
#      claim still fails after the wait limit (VMAF_ADR_LOCK_WAIT_S=3 here).
#
# Usage:   bash scripts/adr/tests/test-next-free-stale-lock.sh
# Exit:    0 = every case behaved as stated; 1 = a case did not.

set -euo pipefail

# shellcheck source=/dev/null
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/../../lib/clean-git-env.sh"

REPO_ROOT="$(git rev-parse --show-toplevel)"
SCRIPT="${REPO_ROOT}/scripts/adr/next-free.sh"
TMPDIR_BASE="$(mktemp -d)"
LIVE_PID=""
FAIL=0

_cleanup() {
  if [ -n "${LIVE_PID}" ]; then
    kill "${LIVE_PID}" 2>/dev/null || printf 'note: holder %s already gone\n' "${LIVE_PID}" >&2
  fi
  local l
  for l in "${TMPDIR_BASE}"/*/lockname; do
    [ -f "${l}" ] || continue
    rm -rf "$(cat "${l}")"
  done
  rm -rf "${TMPDIR_BASE}"
}
trap '_cleanup' EXIT INT TERM

# Build a repository and print the allocator's lock path for it.
_make_repo() {
  local repo="$1"
  mkdir -p "${repo}/docs/adr"
  git -C "${repo}" init --quiet
  touch "${repo}/docs/adr/0600-alpha.md"
  git -C "${repo}" add .
  git -C "${repo}" commit --quiet -m "chore: initial ADR"
  git -C "${repo}" remote add origin "${repo}"
  git -C "${repo}" update-ref refs/remotes/origin/master "$(git -C "${repo}" rev-parse HEAD)"
  local lock
  lock="/tmp/vmaf_adr_claim_lock_$(cd "${repo}/.git" && pwd -P | tr '/' '_')"
  printf '%s\n' "${lock}" >"${repo}/lockname"
  printf '%s\n' "${lock}"
}

# Run one claim; print its exit status, its number ("-" for none) and its
# runtime in seconds.
_claim() {
  local repo="$1" slug="$2" start rc=0
  start="$(date +%s)"
  (cd "${repo}" && bash "${SCRIPT}" --claim "${slug}" >"${repo}/out" 2>"${repo}/err") || rc=$?
  printf '%s %s %s\n' "${rc}" "$(cat "${repo}/out")" $(($(date +%s) - start)) |
    awk 'NF == 2 { $3 = $2; $2 = "-" } { print }'
}

_report() {
  local name="$1" ok="$2" repo="$3"
  if [ "${ok}" -eq 1 ]; then
    printf 'PASS: %s\n' "${name}"
  else
    printf 'FAIL: %s; claim stderr:\n' "${name}" >&2
    sed 's/^/  | /' "${repo}/err" >&2
    FAIL=1
  fi
}

# A process id that is certainly not running: a finished child's.
bash -c 'exit 0' &
DEAD_PID=$!
wait "${DEAD_PID}"

# 1. Owner process gone.
repo="${TMPDIR_BASE}/dead-owner"
lock="$(_make_repo "${repo}")"
mkdir "${lock}"
printf '%s-1-1 %s\n' "${DEAD_PID}" "${repo}" >"${lock}/owner"
read -r rc number secs < <(_claim "${repo}" dead-owner)
ok=0
[ "${rc}" -eq 0 ] && [ "${number}" = "0601" ] && [ "${secs}" -lt 8 ] && [ ! -d "${lock}" ] && ok=1
_report "lock of a finished process is taken over (rc=${rc} number=${number} ${secs}s)" "${ok}" "${repo}"

# 2. Owner file missing.
repo="${TMPDIR_BASE}/no-owner"
lock="$(_make_repo "${repo}")"
mkdir "${lock}"
read -r rc number secs < <(_claim "${repo}" no-owner)
ok=0
[ "${rc}" -eq 0 ] && [ "${number}" = "0601" ] && [ "${secs}" -lt 3 ] && [ ! -d "${lock}" ] && ok=1
_report "lock directory without an owner file is taken (rc=${rc} number=${number} ${secs}s)" "${ok}" "${repo}"

# 3. Owner process alive: the lock stands and the claim fails after the wait.
repo="${TMPDIR_BASE}/live-owner"
lock="$(_make_repo "${repo}")"
sleep 60 &
LIVE_PID=$!
mkdir "${lock}"
printf '%s-1-1 %s\n' "${LIVE_PID}" "${repo}" >"${lock}/owner"
read -r rc number secs < <(VMAF_ADR_LOCK_WAIT_S=3 _claim "${repo}" live-owner)
ok=0
[ "${rc}" -ne 0 ] && [ "${number}" = "-" ] && [ "${secs}" -ge 3 ] && [ -f "${lock}/owner" ] &&
  grep -q "could not acquire ADR allocator lock" "${repo}/err" && ok=1
_report "lock of a running process is respected (rc=${rc} number=${number} ${secs}s)" "${ok}" "${repo}"

if [ "${FAIL}" -ne 0 ]; then
  printf 'FAILED: stale-lock recovery\n' >&2
  exit 1
fi
printf 'All stale-lock cases passed.\n'
