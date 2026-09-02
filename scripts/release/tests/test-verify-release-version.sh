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

# A fixture repo in the post-cut state the verifier now demands (ADR-1151):
# manifest + markers agree, CHANGELOG.md carries exactly one dated release
# heading, the rollover receipt exists, no active fragments or legacy source
# remain, and the one-shot release-please cutover fields are gone.
fixture() {
  local root="$1"
  local manifest_version="${2:-3.2.1}"
  local marker_version="${3:-3.2.1}"
  local changelog_version="${4:-3.2.1}"
  mkdir -p "$root" "$root/changelog.d/releases" "$root/changelog.d/fixed"
  printf '{".":"%s"}\n' "$manifest_version" >"$root/.release-please-manifest.json"
  printf '%s\n' \
    '{"packages":{".":{"extra-files":[' \
    '{"type":"generic","path":"marker-a.txt"},' \
    '{"type":"generic","path":"marker-b.txt"}' \
    ']}}}' >"$root/release-please-config.json"
  printf 'version=%s # x-release-please-version\n' "$marker_version" >"$root/marker-a.txt"
  printf 'version=%s # x-release-please-version\n' "$marker_version" >"$root/marker-b.txt"
  printf '%s\n' \
    '# Changelog' \
    '' \
    '## [Unreleased]' \
    '' \
    "## [$changelog_version] - 2026-08-31" \
    '' \
    '### Fixed' \
    '' \
    '- something' >"$root/CHANGELOG.md"
  printf '{"version":"%s","date":"2026-08-31"}\n' \
    "$changelog_version" >"$root/changelog.d/releases/$changelog_version.json"
}

reject() {
  local description="$1"
  local root="$2"
  local arg_tag="${3:-v3.2.1}"
  if env VMAFX_REPO_ROOT="$root" "$VERIFY" "$arg_tag" >/dev/null 2>&1; then
    check "$description" false
  else
    check "$description" true
  fi
}

good="$scratch/good"
fixture "$good"
check 'matching ordinary tag succeeds' env VMAFX_REPO_ROOT="$good" "$VERIFY" v3.2.1

bad_tag="$scratch/bad-tag"
fixture "$bad_tag"
reject 'prerelease tag is rejected' "$bad_tag" v3.2.1-rc.1

bad_manifest="$scratch/bad-manifest"
fixture "$bad_manifest" 3.2.0 3.2.1
reject 'manifest mismatch is rejected' "$bad_manifest"

bad_marker="$scratch/bad-marker"
fixture "$bad_marker" 3.2.1 3.2.0
reject 'coordinated marker mismatch is rejected' "$bad_marker"

duplicate="$scratch/duplicate"
fixture "$duplicate"
printf 'other=3.2.1 # x-release-please-version\n' >>"$duplicate/marker-a.txt"
reject 'duplicate marker is rejected' "$duplicate"

# --- Changelog-cut assertions (ADR-1151) --------------------------------------
#
# release-please never touches CHANGELOG.md (skip-changelog: true, ADR-1128);
# the operator runs the fragment rollover by hand on the release PR. Nothing
# used to prove that step happened before the tag was published, so these cases
# pin every post-condition of rollover-changelog-fragments.sh.

no_heading="$scratch/no-heading"
fixture "$no_heading"
printf '%s\n' '# Changelog' '' '## [Unreleased]' >"$no_heading/CHANGELOG.md"
reject 'missing dated release heading is rejected' "$no_heading"

undated_heading="$scratch/undated-heading"
fixture "$undated_heading"
printf '%s\n' '# Changelog' '' '## [Unreleased]' '' '## [3.2.1]' \
  >"$undated_heading/CHANGELOG.md"
reject 'undated release heading is rejected' "$undated_heading"

duplicate_heading="$scratch/duplicate-heading"
fixture "$duplicate_heading"
printf '## [3.2.1] - 2026-09-01\n' >>"$duplicate_heading/CHANGELOG.md"
reject 'duplicate release heading is rejected' "$duplicate_heading"

no_receipt="$scratch/no-receipt"
fixture "$no_receipt"
rm -f "$no_receipt/changelog.d/releases/3.2.1.json"
reject 'missing rollover receipt is rejected' "$no_receipt"

wrong_receipt="$scratch/wrong-receipt"
fixture "$wrong_receipt"
printf '{"version":"3.2.0","date":"2026-08-31"}\n' \
  >"$wrong_receipt/changelog.d/releases/3.2.1.json"
reject 'rollover receipt version mismatch is rejected' "$wrong_receipt"

live_fragment="$scratch/live-fragment"
fixture "$live_fragment"
printf -- '- a late fix\n' >"$live_fragment/changelog.d/fixed/late.md"
reject 'active changelog fragment is rejected' "$live_fragment"

legacy_source="$scratch/legacy-source"
fixture "$legacy_source"
printf -- '- legacy body\n' >"$legacy_source/changelog.d/_pre_fragment_legacy.md"
reject 'legacy Unreleased source is rejected' "$legacy_source"

live_release_as="$scratch/live-release-as"
fixture "$live_release_as"
printf '%s\n' \
  '{"packages":{".":{"release-as":"3.2.1","extra-files":[' \
  '{"type":"generic","path":"marker-a.txt"},' \
  '{"type":"generic","path":"marker-b.txt"}' \
  ']}}}' >"$live_release_as/release-please-config.json"
reject 'surviving one-shot release-as is rejected' "$live_release_as"

live_bootstrap="$scratch/live-bootstrap"
fixture "$live_bootstrap"
printf '%s\n' \
  '{"bootstrap-sha":"deadbeef","packages":{".":{"extra-files":[' \
  '{"type":"generic","path":"marker-a.txt"},' \
  '{"type":"generic","path":"marker-b.txt"}' \
  ']}}}' >"$live_bootstrap/release-please-config.json"
reject 'surviving one-shot bootstrap-sha is rejected' "$live_bootstrap"

missing_changelog="$scratch/missing-changelog"
fixture "$missing_changelog"
rm -f "$missing_changelog/CHANGELOG.md"
reject 'missing CHANGELOG.md is rejected' "$missing_changelog"

first_release="$scratch/first-release"
fixture "$first_release" 1.0.0 1.0.0 1.0.0
check 'first 1.0.0 release on the fresh number line succeeds' \
  env VMAFX_REPO_ROOT="$first_release" "$VERIFY" v1.0.0

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
reject 'selected tag away from HEAD is rejected' "$tagged"

printf '\n=== Results: %d passed, %d failed ===\n' "$pass" "$fail"
[[ "$fail" -eq 0 ]]
