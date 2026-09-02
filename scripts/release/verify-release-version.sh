#!/usr/bin/env bash
# Verify that an ordinary release tag matches every coordinated version marker.
#
# Copyright 2026 Lusoris
# Copyright 2026 Claude (Anthropic)
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${VMAFX_REPO_ROOT:-$(cd -- "$SCRIPT_DIR/../.." && pwd)}"
CONFIG="$REPO_ROOT/release-please-config.json"
MANIFEST="$REPO_ROOT/.release-please-manifest.json"

usage() {
  printf 'Usage: verify-release-version.sh vMAJOR.MINOR.PATCH\n'
}

if [[ $# -ne 1 ]]; then
  usage >&2
  exit 64
fi

tag="$1"
if [[ ! "$tag" =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
  printf 'ERROR: release tag must be ordinary SemVer vMAJOR.MINOR.PATCH: %s\n' "$tag" >&2
  exit 64
fi
version="${tag#v}"

for required in "$CONFIG" "$MANIFEST"; do
  if [[ ! -f "$required" ]]; then
    printf 'ERROR: required release input missing: %s\n' "$required" >&2
    exit 1
  fi
done

manifest_version="$(jq -er '.["."]' "$MANIFEST")"
if [[ "$manifest_version" != "$version" ]]; then
  printf 'ERROR: root manifest is %s, expected %s from %s\n' \
    "$manifest_version" "$version" "$tag" >&2
  exit 1
fi

mapfile -t marker_files < <(
  jq -er '.packages["."]."extra-files"[] | if type == "string" then . else .path end' \
    "$CONFIG"
)
if [[ ${#marker_files[@]} -eq 0 ]]; then
  printf 'ERROR: release config has no coordinated version files\n' >&2
  exit 1
fi

for relative_path in "${marker_files[@]}"; do
  marker_path="$REPO_ROOT/$relative_path"
  if [[ ! -f "$marker_path" ]]; then
    printf 'ERROR: coordinated version file missing: %s\n' "$relative_path" >&2
    exit 1
  fi
  marker_count="$(grep -Ec 'x-release-please-version' "$marker_path" || true)"
  marker_line="$(grep -E 'x-release-please-version' "$marker_path" || true)"
  marker_version="$(printf '%s\n' "$marker_line" | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+' || true)"
  if [[ "$marker_count" -ne 1 || "$marker_version" != "$version" ]]; then
    printf 'ERROR: %s does not contain exactly one %s release marker\n' \
      "$relative_path" "$version" >&2
    exit 1
  fi
done

if [[ -d "$REPO_ROOT/.git" || -f "$REPO_ROOT/.git" ]]; then
  if [[ "$(git -C "$REPO_ROOT" tag --points-at HEAD --list "$tag")" != "$tag" ]]; then
    printf 'ERROR: checked-out commit is not tagged exactly %s\n' "$tag" >&2
    exit 1
  fi
fi

printf 'Release tag %s matches the manifest and %d coordinated markers.\n' \
  "$tag" "${#marker_files[@]}"
