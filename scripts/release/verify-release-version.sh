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
CHANGELOG="$REPO_ROOT/CHANGELOG.md"
FRAG_ROOT="$REPO_ROOT/changelog.d"

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

for required in "$CONFIG" "$MANIFEST" "$CHANGELOG"; do
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

# --- The changelog cut actually ran (ADR-1151) --------------------------------
#
# release-please has `skip-changelog: true`, so nothing in the automated path
# touches CHANGELOG.md; the fragment renderer owns it (ADR-1128) and the cut is
# a hand-run step on the release PR. Before this block nothing downstream
# verified that the operator had run it, so a tag could publish a CHANGELOG
# whose newest section was still `## [Unreleased]` with 1,500 live fragments
# underneath it. Every assertion here is fail-closed and mirrors the
# post-conditions of scripts/release/rollover-changelog-fragments.sh.

escaped_version="${version//./\\.}"
heading_count="$(grep -Ec "^## \\[${escaped_version}\\] - [0-9]{4}-[0-9]{2}-[0-9]{2}$" \
  "$CHANGELOG" || true)"
if [[ "$heading_count" -ne 1 ]]; then
  printf 'ERROR: CHANGELOG.md must contain exactly one "## [%s] - YYYY-MM-DD" heading (found %s)\n' \
    "$version" "$heading_count" >&2
  printf '       Run scripts/release/rollover-changelog-fragments.sh on the release PR.\n' >&2
  exit 1
fi

receipt="$FRAG_ROOT/releases/$version.json"
if [[ ! -f "$receipt" ]]; then
  printf 'ERROR: missing changelog rollover receipt: changelog.d/releases/%s.json\n' \
    "$version" >&2
  exit 1
fi
receipt_version="$(jq -er '.version' "$receipt")"
if [[ "$receipt_version" != "$version" ]]; then
  printf 'ERROR: rollover receipt records version %s, expected %s\n' \
    "$receipt_version" "$version" >&2
  exit 1
fi

active_fragments=0
for section in added changed deprecated removed fixed security; do
  if [[ -d "$FRAG_ROOT/$section" ]]; then
    count="$(find "$FRAG_ROOT/$section" -maxdepth 1 -type f -name '*.md' ! -name '.*' | wc -l)"
    active_fragments=$((active_fragments + count))
  fi
done
if [[ "$active_fragments" -ne 0 ]]; then
  printf 'ERROR: %d active changelog fragment(s) remain; the cut is stale\n' \
    "$active_fragments" >&2
  exit 1
fi
if [[ -f "$FRAG_ROOT/_pre_fragment_legacy.md" ]]; then
  printf 'ERROR: changelog.d/_pre_fragment_legacy.md remains; the cut is stale\n' >&2
  exit 1
fi

# One-shot release-please cutover fields are deleted by the rollover. If either
# survives to tag time the config would pin every future release to this
# version (release-as) or truncate the next release's notes (bootstrap-sha).
one_time_fields="$(jq -r '[has("bootstrap-sha"),
  (.packages["."] | has("release-as"))] | map(select(.)) | length' "$CONFIG")"
if [[ "$one_time_fields" -ne 0 ]]; then
  printf 'ERROR: one-shot release-please fields (bootstrap-sha / release-as) survive at tag time\n' >&2
  exit 1
fi

if [[ -d "$REPO_ROOT/.git" || -f "$REPO_ROOT/.git" ]]; then
  if [[ "$(git -C "$REPO_ROOT" tag --points-at HEAD --list "$tag")" != "$tag" ]]; then
    printf 'ERROR: checked-out commit is not tagged exactly %s\n' "$tag" >&2
    exit 1
  fi
fi

printf 'Release tag %s matches the manifest, %d coordinated markers, and a completed changelog cut.\n' \
  "$tag" "${#marker_files[@]}"
