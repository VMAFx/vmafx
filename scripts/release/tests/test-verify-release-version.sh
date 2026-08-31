#!/usr/bin/env bash
# Regression tests for the coordinated release-version preflight.
#
# Copyright 2026 Lusoris
# Copyright 2026 Claude (Anthropic)
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VERIFY="$SCRIPT_DIR/../verify-release-version.sh"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT INT TERM
pass=0
fail=0

check() {
  local description="$1"
  shift
  if "$@"; then
    printf 'PASS: %s\n' "$description"
    pass=$((pass + 1))
  else
    printf 'FAIL: %s\n' "$description" >&2
    fail=$((fail + 1))
  fi
}

fixture() {
  local root="$1"
  local manifest_version="${2:-3.2.1}"
  local marker_version="${3:-3.2.1}"
  mkdir -p "$root"
  printf '{".":"%s"}\n' "$manifest_version" >"$root/.release-please-manifest.json"
  printf '%s\n' \
    '{"packages":{".":{"extra-files":[' \
    '{"type":"generic","path":"marker-a.txt"},' \
    '{"type":"generic","path":"marker-b.txt"}' \
    ']}}}' >"$root/release-please-config.json"
  printf 'version=%s # x-release-please-version\n' "$marker_version" >"$root/marker-a.txt"
  printf 'version=%s # x-release-please-version\n' "$marker_version" >"$root/marker-b.txt"
}

good="$scratch/good"
fixture "$good"
check 'matching ordinary tag succeeds' env VMAFX_REPO_ROOT="$good" "$VERIFY" v3.2.1

bad_tag="$scratch/bad-tag"
fixture "$bad_tag"
if env VMAFX_REPO_ROOT="$bad_tag" "$VERIFY" v3.2.1-rc.1 >/dev/null 2>&1; then
  check 'prerelease tag is rejected' false
else
  check 'prerelease tag is rejected' true
fi

bad_manifest="$scratch/bad-manifest"
fixture "$bad_manifest" 3.2.0 3.2.1
if env VMAFX_REPO_ROOT="$bad_manifest" "$VERIFY" v3.2.1 >/dev/null 2>&1; then
  check 'manifest mismatch is rejected' false
else
  check 'manifest mismatch is rejected' true
fi

bad_marker="$scratch/bad-marker"
fixture "$bad_marker" 3.2.1 3.2.0
if env VMAFX_REPO_ROOT="$bad_marker" "$VERIFY" v3.2.1 >/dev/null 2>&1; then
  check 'coordinated marker mismatch is rejected' false
else
  check 'coordinated marker mismatch is rejected' true
fi

duplicate="$scratch/duplicate"
fixture "$duplicate"
printf 'other=3.2.1 # x-release-please-version\n' >>"$duplicate/marker-a.txt"
if env VMAFX_REPO_ROOT="$duplicate" "$VERIFY" v3.2.1 >/dev/null 2>&1; then
  check 'duplicate marker is rejected' false
else
  check 'duplicate marker is rejected' true
fi

tagged="$scratch/tagged"
fixture "$tagged"
git -C "$tagged" init -q
git -C "$tagged" add .
git -C "$tagged" -c user.name=test -c user.email=test@example.invalid \
  commit -q -m fixture
git -C "$tagged" tag v3.2.1
check 'selected tag at HEAD succeeds' env VMAFX_REPO_ROOT="$tagged" "$VERIFY" v3.2.1
printf 'later\n' >"$tagged/later.txt"
git -C "$tagged" add later.txt
git -C "$tagged" -c user.name=test -c user.email=test@example.invalid \
  commit -q -m later
if env VMAFX_REPO_ROOT="$tagged" "$VERIFY" v3.2.1 >/dev/null 2>&1; then
  check 'selected tag away from HEAD is rejected' false
else
  check 'selected tag away from HEAD is rejected' true
fi

printf '\n=== Results: %d passed, %d failed ===\n' "$pass" "$fail"
[[ "$fail" -eq 0 ]]
