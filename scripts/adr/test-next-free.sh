#!/usr/bin/env bash
# scripts/adr/test-next-free.sh — smoke tests for the atomic ADR allocator.
#
# Verifies:
#   1. Sequential claims return distinct, increasing numbers.
#   2. Parallel (concurrent) claims return distinct numbers (race-free).
#   3. --release removes the stub and frees the slot.
#   4. Invalid slugs are rejected.
#   5. Read-only (no-arg) mode is unaffected by stubs.
#
# Usage:
#   bash scripts/adr/test-next-free.sh
#
# Exit code: 0 = all tests passed; non-zero = failure.
#
# See ADR-0535 (docs/adr/0535-adr-atomic-allocator.md).

set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
SCRIPT="${REPO_ROOT}/scripts/adr/next-free.sh"
ADR_DIR="${REPO_ROOT}/docs/adr"

# ── helpers ───────────────────────────────────────────────────────────────────

PASS=0
FAIL=0

_pass() {
  printf 'PASS: %s\n' "$1"
  PASS=$((PASS + 1))
}

_fail() {
  printf 'FAIL: %s\n' "$1" >&2
  FAIL=$((FAIL + 1))
}

# Clean up any stubs we create, even on early exit.
_STUBS_TO_CLEAN=()
_cleanup() {
  for s in "${_STUBS_TO_CLEAN[@]+"${_STUBS_TO_CLEAN[@]}"}"; do
    rm -f "${s}" 2>/dev/null || true
  done
  # Remove the lock directory if we left it behind.
  _repo_key="$(printf '%s' "${REPO_ROOT}" | tr '/' '_')"
  rmdir "/tmp/vmaf_adr_claim_lock_${_repo_key}" 2>/dev/null || true
}
trap '_cleanup' EXIT INT TERM

# ── test 1: sequential claims are distinct and increasing ─────────────────────

N1=$(bash "${SCRIPT}" --claim test-seq-a 2>/dev/null)
_STUBS_TO_CLEAN+=("${ADR_DIR}/${N1}-test-seq-a.md.stub")

N2=$(bash "${SCRIPT}" --claim test-seq-b 2>/dev/null)
_STUBS_TO_CLEAN+=("${ADR_DIR}/${N2}-test-seq-b.md.stub")

N3=$(bash "${SCRIPT}" --claim test-seq-c 2>/dev/null)
_STUBS_TO_CLEAN+=("${ADR_DIR}/${N3}-test-seq-c.md.stub")

if [ "${N1}" != "${N2}" ] && [ "${N2}" != "${N3}" ] && [ "${N1}" != "${N3}" ]; then
  _pass "sequential claims are distinct: N1=${N1} N2=${N2} N3=${N3}"
else
  _fail "sequential claims collided: N1=${N1} N2=${N2} N3=${N3}"
fi

if [ "$((10#${N2}))" -gt "$((10#${N1}))" ] &&
  [ "$((10#${N3}))" -gt "$((10#${N2}))" ]; then
  _pass "sequential claims are increasing"
else
  _fail "sequential claims not monotone: N1=${N1} N2=${N2} N3=${N3}"
fi

# ── test 2: parallel claims are distinct (race test) ──────────────────────────

T1=$(mktemp)
T2=$(mktemp)

bash "${SCRIPT}" --claim test-par-a >"${T1}" 2>/dev/null &
PID1=$!
bash "${SCRIPT}" --claim test-par-b >"${T2}" 2>/dev/null &
PID2=$!
wait "${PID1}"
wait "${PID2}"

PN1=$(cat "${T1}")
PN2=$(cat "${T2}")
rm -f "${T1}" "${T2}"

_STUBS_TO_CLEAN+=("${ADR_DIR}/${PN1}-test-par-a.md.stub")
_STUBS_TO_CLEAN+=("${ADR_DIR}/${PN2}-test-par-b.md.stub")

if [ -n "${PN1}" ] && [ -n "${PN2}" ] && [ "${PN1}" != "${PN2}" ]; then
  _pass "parallel claims are distinct: PN1=${PN1} PN2=${PN2}"
else
  _fail "parallel claims collided or empty: PN1=${PN1} PN2=${PN2}"
fi

# ── test 3: stub files exist on disk ─────────────────────────────────────────

for stub in \
  "${ADR_DIR}/${N1}-test-seq-a.md.stub" \
  "${ADR_DIR}/${N2}-test-seq-b.md.stub" \
  "${ADR_DIR}/${N3}-test-seq-c.md.stub" \
  "${ADR_DIR}/${PN1}-test-par-a.md.stub" \
  "${ADR_DIR}/${PN2}-test-par-b.md.stub"; do
  if [ -f "${stub}" ]; then
    _pass "stub exists: $(basename "${stub}")"
  else
    _fail "stub missing: ${stub}"
  fi
done

# ── test 4: --release removes the stub ───────────────────────────────────────

released=$(bash "${SCRIPT}" --release "${N1}" 2>/dev/null)
if printf '%s' "${released}" | grep -q "released ${N1}"; then
  _pass "--release output contains 'released ${N1}'"
else
  _fail "--release output wrong: ${released}"
fi
if [ ! -f "${ADR_DIR}/${N1}-test-seq-a.md.stub" ]; then
  _pass "stub was removed by --release"
else
  _fail "stub still exists after --release"
fi

# ── test 5: read-only mode is unaffected by stubs ────────────────────────────

Q=$(bash "${SCRIPT}" 2>/dev/null)
if printf '%s' "${Q}" | grep -qE '^[0-9]{4}$'; then
  _pass "read-only mode returns a 4-digit number: ${Q}"
else
  _fail "read-only mode returned unexpected output: ${Q}"
fi

# ── test 6: invalid slug is rejected ─────────────────────────────────────────

if bash "${SCRIPT}" --claim "INVALID SLUG" >/dev/null 2>&1; then
  _fail "invalid slug was accepted (should have been rejected)"
else
  _pass "invalid slug correctly rejected"
fi

# ── summary ───────────────────────────────────────────────────────────────────

printf '\n%d passed, %d failed\n' "${PASS}" "${FAIL}"
[ "${FAIL}" -eq 0 ]
