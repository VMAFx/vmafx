#!/usr/bin/env bash
# Test harness for scripts/ci/release-pr-exempt.sh (ADR-1151).
#
# Runs the predicate with every combination of head ref and author identity
# that the release pipeline can produce, and asserts the emitted `exempt=`
# value. Hermetic: only `mktemp -d` is written.
#
# Usage: bash scripts/ci/tests/test-release-pr-exempt.sh
#
# Exit 0 on all-pass, 1 on any failure.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
GATE="$SCRIPT_DIR/../release-pr-exempt.sh"

if [[ ! -f "$GATE" ]]; then
  printf 'ERROR: %s not found\n' "$GATE" >&2
  exit 1
fi

TMPDIR_TESTS="$(mktemp -d)"
cleanup() {
  rm -rf -- "$TMPDIR_TESTS"
}
trap cleanup EXIT

pass=0
fail=0

# expect <label> <want-exempt> <head-ref> <author> <author-type>
expect() {
  local label="$1" want="$2" head_ref="$3" author="$4" author_type="$5"
  local out_file="$TMPDIR_TESTS/out"
  local log_file="$TMPDIR_TESTS/log"
  : >"$out_file"
  local rc=0
  HEAD_REF="$head_ref" \
    PR_AUTHOR="$author" \
    PR_AUTHOR_TYPE="$author_type" \
    GITHUB_OUTPUT="$out_file" \
    bash "$GATE" >"$log_file" 2>&1 || rc=$?
  if [[ $rc -ne 0 ]]; then
    printf 'FAIL %s: exited %d (must always exit 0)\n' "$label" "$rc"
    sed 's/^/      /' "$log_file"
    fail=$((fail + 1))
    return
  fi
  local got
  got="$(sed -n 's/^exempt=//p' "$out_file")"
  if [[ "$got" != "$want" ]]; then
    printf 'FAIL %s: GITHUB_OUTPUT exempt=%s, want %s\n' "$label" "$got" "$want"
    sed 's/^/      /' "$log_file"
    fail=$((fail + 1))
    return
  fi
  if ! grep -q "exempt=${want}" "$log_file"; then
    printf 'FAIL %s: stdout does not report exempt=%s\n' "$label" "$want"
    sed 's/^/      /' "$log_file"
    fail=$((fail + 1))
    return
  fi
  printf 'PASS %s\n' "$label"
  pass=$((pass + 1))
}

RP_REF="release-please--branches--master--components--vmafx"

# The real release PR, both before and after the release-bot App exists.
expect "release branch, github-actions bot" true \
  "$RP_REF" "github-actions[bot]" "Bot"
expect "release branch, release-bot App" true \
  "$RP_REF" "vmafx-release-bot[bot]" "Bot"

# `user.type` missing from the payload: the `[bot]` login suffix still
# identifies the author.
expect "release branch, bot login without user.type" true \
  "$RP_REF" "github-actions[bot]" ""

# Branch name alone must never disarm a required gate.
expect "release-shaped branch, human author" false \
  "$RP_REF" "lusoris" "User"
expect "release-shaped branch, no author info" false \
  "$RP_REF" "" ""

# Ordinary PRs, human and bot alike, keep every gate armed.
expect "ordinary branch, human author" false \
  "fix/release-please-setup" "lusoris" "User"
expect "ordinary branch, renovate bot" false \
  "renovate/anyio-4.x" "renovate[bot]" "Bot"
expect "branch merely containing the prefix" false \
  "chore/release-please--notes" "lusoris" "User"

# Non-pull_request events hand the workflow an empty head ref.
expect "empty head ref" false "" "" ""

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[[ $fail -eq 0 ]]
