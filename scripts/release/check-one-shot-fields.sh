#!/usr/bin/env bash
# Keep release-please's one-shot cutover fields from outliving the release they
# were set for.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# `release-as` and `bootstrap-sha` are one-shot fields (ADR-1151). release-as is
# applied before commit analysis, so a copy left in place pins every later
# release to the same version and swallows feat / BREAKING bumps.
# rollover-changelog-fragments.sh deletes both in the release-merge pull
# request; this gate makes sure neither comes back. Force a later version with
# a `Release-As: X.Y.Z` commit footer instead, which is one-shot by itself.
#
# The rules, read from the root package of the manifest and the config:
#
#   - a final manifest version (no `-` suffix, 0.x included) carries neither
#     field: the release the one-shot was set for has landed;
#   - a candidate manifest (`1.0.0-rc.N`) uses `versioning: prerelease`, so
#     release-please counts candidates (ADR-1348: `default` turns a fix on
#     1.0.0-rc.1 into 1.0.1-rc.1);
#   - the one exception is the 0.x restart (ADR-3292): a candidate manifest
#     with `versioning: default` is allowed only while `release-as` is set,
#     which moves the next release off the candidate line in one step.
#
# REPO_ROOT defaults to this checkout; VMAFX_REPO_ROOT points it at a fixture.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${VMAFX_REPO_ROOT:-$(cd -- "$SCRIPT_DIR/../.." && pwd)}"
CONFIG="$REPO_ROOT/release-please-config.json"
MANIFEST="$REPO_ROOT/.release-please-manifest.json"

manifest_version="$(jq -er '.["."]' "$MANIFEST")"
versioning="$(jq -er '.packages["."].versioning // "default"' "$CONFIG")"
release_as="$(jq -er '.packages["."]["release-as"] // ""' "$CONFIG")"
survivors="$(jq -er '[
  (if has("bootstrap-sha") then "bootstrap-sha" else empty end),
  (if (.packages["."] | has("release-as")) then "release-as" else empty end)
] | join(", ")' "$CONFIG")"
echo "root manifest version: $manifest_version, versioning: $versioning"

if [[ "$manifest_version" == *-* ]]; then
  if [[ "$versioning" == prerelease ]]; then
    echo "::notice::manifest $manifest_version is a candidate counted by prerelease versioning"
    exit 0
  fi
  if [[ -n "$release_as" ]]; then
    echo "::notice::manifest $manifest_version leaves the candidate line through release-as $release_as (ADR-3292)"
    exit 0
  fi
  echo "::error::manifest $manifest_version is a release candidate but release-please versioning is '$versioning' and no release-as is set; set \"versioning\": \"prerelease\" (ADR-1348)"
  exit 1
fi

if [[ -n "$survivors" ]]; then
  echo "::error::one-shot release-please field(s) survive at manifest $manifest_version: $survivors"
  echo "Remove them from release-please-config.json (see ADR-1151);"
  echo "use a 'Release-As: X.Y.Z' commit footer to force a version instead."
  exit 1
fi
echo "No one-shot release-please cutover fields remain."
