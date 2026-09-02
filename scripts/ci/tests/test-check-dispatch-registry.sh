#!/usr/bin/env bash
# Test harness for scripts/ci/check-dispatch-registry.sh.
#
# Usage: bash scripts/ci/tests/test-check-dispatch-registry.sh
#
# Exit 0 on all-pass, 1 on any failure.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CHECK_SCRIPT="$SCRIPT_DIR/../check-dispatch-registry.sh"
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
out=$(bash "$CHECK_SCRIPT" "$ROOT" 2>&1) || exit_code=$?
assert_eq "Real tree exits 0" "0" "$exit_code"
assert_eq "Real tree outputs PASS" "1" "$(grep -cF 'PASS: all backend symbols present' <<<"$out")"

echo "=== Test 2: Missing symbol triggers failure ==="
mock_tree="$(mktemp -d -p "$TMPDIR_TESTS")"
mkdir -p "$mock_tree/core/src/feature/cuda"
cat <<'MOCK_EOF' >"$mock_tree/core/src/feature/cuda/fex.c"
VmafFeatureExtractor vmaf_fex_sample_cuda = {
    .name = "sample_cuda",
};
MOCK_EOF
cat <<'MOCK_EOF' >"$mock_tree/core/src/feature/feature_extractor.cpp"
static VmafFeatureExtractor *feature_extractor_list[] = {
    &vmaf_fex_other_cuda,
    NULL,
};
MOCK_EOF

exit_code=0
out=$(bash "$CHECK_SCRIPT" "$mock_tree" 2>&1) || exit_code=$?
assert_eq "Mock tree with missing symbol exits 1" "1" "$exit_code"
assert_eq "Missing message emitted" "1" "$(grep -cF 'MISSING: vmaf_fex_sample_cuda not in feature_extractor_list[]' <<<"$out")"

echo "=== Test 3: Duplicate symbol triggers warning ==="
mock_tree_dup="$(mktemp -d -p "$TMPDIR_TESTS")"
mkdir -p "$mock_tree_dup/core/src/feature/cuda"
cat <<'MOCK_EOF' >"$mock_tree_dup/core/src/feature/cuda/fex.c"
VmafFeatureExtractor vmaf_fex_sample_cuda = {
    .name = "sample_cuda",
};
MOCK_EOF
cat <<'MOCK_EOF' >"$mock_tree_dup/core/src/feature/feature_extractor.cpp"
static VmafFeatureExtractor *feature_extractor_list[] = {
    &vmaf_fex_sample_cuda,
    &vmaf_fex_sample_cuda,
    NULL,
};
MOCK_EOF

exit_code=0
out=$(bash "$CHECK_SCRIPT" "$mock_tree_dup" 2>&1) || exit_code=$?
assert_eq "Mock tree with duplicates exits 0" "0" "$exit_code"
assert_eq "Warning message emitted" "1" "$(grep -cF 'WARNING: vmaf_fex_sample_cuda appears 2 times' <<<"$out")"

echo "=== Test 4: Missing feature_extractor.cpp triggers error ==="
mock_tree_nofex="$(mktemp -d -p "$TMPDIR_TESTS")"
exit_code=0
out=$(bash "$CHECK_SCRIPT" "$mock_tree_nofex" 2>&1) || exit_code=$?
assert_eq "Missing feature_extractor.cpp exits 1" "1" "$exit_code"
assert_eq "Error message emitted" "1" "$(grep -cF 'ERROR: feature_extractor.cpp not found' <<<"$out")"

echo ""
echo "=== Summary: $pass passed, $fail failed ==="
if [[ "$fail" -gt 0 ]]; then
  exit 1
fi
