#!/usr/bin/env bash
# Test harness for scripts/ci/check-no-non-header-includes.sh.
#
# Usage: bash scripts/ci/tests/test-check-no-non-header-includes.sh
#
# Exit 0 on all-pass, 1 on any failure.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CHECK_SCRIPT="$SCRIPT_DIR/../check-no-non-header-includes.sh"
ROOT="$(cd -- "$SCRIPT_DIR/../../.." && pwd)"

if [[ ! -f "$CHECK_SCRIPT" ]]; then
  printf 'ERROR: %s not found\n' "$CHECK_SCRIPT" >&2
  exit 1
fi

TMPDIR_TESTS="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_TESTS"' EXIT

pass=0
fail=0

assert_eq() {
  local desc="$1"
  local expected="$2"
  local actual="$3"
  if [[ "$expected" == "$actual" ]]; then
    printf '  PASS: %s\n' "$desc"
    pass=$((pass + 1))
  else
    printf '  FAIL: %s\n' "$desc" >&2
    printf '    expected: %s\n' "$expected" >&2
    printf '    actual:   %s\n' "$actual" >&2
    fail=$((fail + 1))
  fi
}

echo "=== Test 1: Real tree passes ==="
out=""
exit_code=0
out=$(bash "$CHECK_SCRIPT" --root "$ROOT" 2>&1) || exit_code=$?
assert_eq "Real tree exits 0" "0" "$exit_code"
assert_eq "Real tree outputs PASS" "1" "$(grep -cF 'PASS: no unallowlisted non-header includes' <<<"$out")"

echo "=== Test 2: Grandfathered allowlist file with .c include passes ==="
mock_tree="$(mktemp -d -p "$TMPDIR_TESTS")"
mkdir -p "$mock_tree/core/test"
cat <<'MOCK_EOF' >"$mock_tree/core/test/test_ciede.c"
#include "test.h"
#include "feature/ciede.c"
MOCK_EOF
out=""
exit_code=0
out=$(bash "$CHECK_SCRIPT" --root "$mock_tree" 2>&1) || exit_code=$?
assert_eq "Allowlisted file exits 0" "0" "$exit_code"

echo "=== Test 3: Unallowlisted file with .c include fails ==="
cat <<'MOCK_EOF' >"$mock_tree/core/test/test_forbidden.c"
#include "test.h"
#include "libvmaf.c"
MOCK_EOF
out=""
exit_code=0
out=$(bash "$CHECK_SCRIPT" --root "$mock_tree" 2>&1) || exit_code=$?
assert_eq "Unallowlisted .c include exits 1" "1" "$exit_code"
assert_eq "Outputs error message" "1" "$(grep -cF 'ERROR:' <<<"$out")"

echo "=== Test 4: Unallowlisted file with .cpp include fails ==="
rm -f "$mock_tree/core/test/test_forbidden.c"
cat <<'MOCK_EOF' >"$mock_tree/core/test/test_forbidden.cpp"
#include "test.h"
#include "feature/something.cpp"
MOCK_EOF
out=""
exit_code=0
out=$(bash "$CHECK_SCRIPT" --root "$mock_tree" 2>&1) || exit_code=$?
assert_eq "Unallowlisted .cpp include exits 1" "1" "$exit_code"

echo "=== Test 5: Specific clean file argument passes ==="
cat <<'MOCK_EOF' >"$mock_tree/core/test/test_clean.c"
#include "test.h"
#include "libvmaf_priv.h"
MOCK_EOF
out=""
exit_code=0
out=$(bash "$CHECK_SCRIPT" --root "$mock_tree" "core/test/test_clean.c" 2>&1) || exit_code=$?
assert_eq "Specific clean file exits 0" "0" "$exit_code"

echo "=== Summary ==="
printf 'Total passed: %d, Total failed: %d\n' "$pass" "$fail"
if [[ "$fail" -gt 0 ]]; then
  exit 1
fi
exit 0
