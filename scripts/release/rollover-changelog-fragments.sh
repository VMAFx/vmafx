#!/usr/bin/env bash
# Convert the rendered Unreleased changelog into one immutable release section.
#
# Run this only on a generated release-please PR after its manifest and every
# x-release-please-version marker have reached the requested version. The
# fragment renderer remains the sole CHANGELOG owner; release-please is set to
# skip its built-in changelog updater.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${VMAFX_REPO_ROOT:-$(cd -- "$SCRIPT_DIR/../.." && pwd)}"
FRAG_ROOT="$REPO_ROOT/changelog.d"
CHANGELOG="$REPO_ROOT/CHANGELOG.md"
CONFIG="$REPO_ROOT/release-please-config.json"
MANIFEST="$REPO_ROOT/.release-please-manifest.json"
CONCAT="$SCRIPT_DIR/concat-changelog-fragments.sh"
if [[ -n "${VMAFX_REPO_ROOT:-}" ]]; then
  CONCAT="$REPO_ROOT/scripts/release/concat-changelog-fragments.sh"
fi

usage() {
  cat <<'EOF'
Usage: rollover-changelog-fragments.sh --version X.Y.Z --date YYYY-MM-DD

Versions the exact rendered Unreleased body, removes the active fragment
sources, retires one-time release-please cutover fields, and writes
changelog.d/releases/X.Y.Z.json as a verification receipt.
EOF
}

version=""
release_date=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --version)
      [[ $# -ge 2 ]] || {
        usage >&2
        exit 64
      }
      version="$2"
      shift 2
      ;;
    --date)
      [[ $# -ge 2 ]] || {
        usage >&2
        exit 64
      }
      release_date="$2"
      shift 2
      ;;
    --help | -h)
      usage
      exit 0
      ;;
    *)
      printf 'ERROR: unknown argument: %s\n' "$1" >&2
      usage >&2
      exit 64
      ;;
  esac
done

if [[ ! "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
  printf 'ERROR: --version must be ordinary SemVer X.Y.Z\n' >&2
  exit 64
fi
if [[ ! "$release_date" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]]; then
  printf 'ERROR: --date must be YYYY-MM-DD\n' >&2
  exit 64
fi
if [[ "$(date -u -d "$release_date" +%F 2>/dev/null || true)" != "$release_date" ]]; then
  printf 'ERROR: --date is not a real UTC calendar date\n' >&2
  exit 64
fi

for required in "$CHANGELOG" "$CONFIG" "$MANIFEST" "$CONCAT"; do
  if [[ ! -f "$required" ]]; then
    printf 'ERROR: required release input missing: %s\n' "$required" >&2
    exit 1
  fi
done

if [[ -d "$REPO_ROOT/.git" || -f "$REPO_ROOT/.git" ]]; then
  if ! git -C "$REPO_ROOT" diff --quiet || ! git -C "$REPO_ROOT" diff --cached --quiet; then
    printf 'ERROR: rollover requires a clean working tree\n' >&2
    exit 1
  fi
fi

manifest_version="$(jq -er '.["."]' "$MANIFEST")"
if [[ "$manifest_version" != "$version" ]]; then
  printf 'ERROR: root manifest is %s, expected %s\n' "$manifest_version" "$version" >&2
  exit 1
fi

mapfile -t marker_files < <(
  jq -er '.packages["."]."extra-files"[] | if type == "string" then . else .path end' "$CONFIG"
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
  marker_line="$(grep -E 'x-release-please-version' "$marker_path" || true)"
  marker_count="$(grep -Ec 'x-release-please-version' "$marker_path" || true)"
  marker_version="$(printf '%s\n' "$marker_line" | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+' || true)"
  if [[ "$marker_count" -ne 1 || "$marker_version" != "$version" ]]; then
    printf 'ERROR: %s does not contain exactly one %s release marker\n' \
      "$relative_path" "$version" >&2
    exit 1
  fi
done

release_as="$(jq -r '.packages["."]."release-as" // empty' "$CONFIG")"
if [[ -n "$release_as" && "$release_as" != "$version" ]]; then
  printf 'ERROR: one-time release-as is %s, expected %s\n' "$release_as" "$version" >&2
  exit 1
fi

if [[ "$(grep -Ec '^## \[Unreleased\]' "$CHANGELOG")" -ne 1 ]]; then
  printf 'ERROR: CHANGELOG.md must contain exactly one Unreleased heading\n' >&2
  exit 1
fi
escaped_version="${version//./\\.}"
if grep -Eq "^## \\[${escaped_version}\\] - " "$CHANGELOG"; then
  receipt="$FRAG_ROOT/releases/$version.json"
  active_after=0
  for section in added changed deprecated removed fixed security; do
    if [[ -d "$FRAG_ROOT/$section" ]]; then
      count="$(find "$FRAG_ROOT/$section" -maxdepth 1 -type f -name '*.md' ! -name '.*' | wc -l)"
      active_after=$((active_after + count))
    fi
  done
  [[ -f "$FRAG_ROOT/_pre_fragment_legacy.md" ]] && active_after=$((active_after + 1))
  one_time_fields="$(jq '[has("bootstrap-sha"), (.packages["."] | has("release-as"))] |
    map(select(.)) | length' "$CONFIG")"
  if [[ "$active_after" -eq 0 && -f "$receipt" && "$one_time_fields" -eq 0 ]]; then
    printf 'Changelog release %s is already rolled over.\n' "$version"
    exit 0
  fi
  printf 'ERROR: release heading exists but sources, receipt, or cutover config are inconsistent\n' >&2
  exit 1
fi

"$CONCAT" --check

sources=()
for section in added changed deprecated removed fixed security; do
  if [[ -d "$FRAG_ROOT/$section" ]]; then
    while IFS= read -r -d '' fragment; do
      sources+=("$fragment")
    done < <(find "$FRAG_ROOT/$section" -maxdepth 1 -type f -name '*.md' ! -name '.*' -print0)
  fi
done
if [[ -f "$FRAG_ROOT/_pre_fragment_legacy.md" ]]; then
  sources+=("$FRAG_ROOT/_pre_fragment_legacy.md")
fi
if [[ ${#sources[@]} -eq 0 ]]; then
  printf 'ERROR: no active changelog sources to release\n' >&2
  exit 1
fi

tmp_body="$(mktemp)"
tmp_changelog="$(mktemp)"
tmp_config="$(mktemp)"
trap 'rm -f "$tmp_body" "$tmp_changelog" "$tmp_config"' EXIT
"$CONCAT" >"$tmp_body"
if [[ ! -s "$tmp_body" ]]; then
  printf 'ERROR: rendered Unreleased body is empty\n' >&2
  exit 1
fi
body_sha256="$(sha256sum "$tmp_body" | cut -d' ' -f1)"

awk -v version="$version" -v release_date="$release_date" '
    /^## \[Unreleased\]/ {
        print
        print ""
        print "## [" version "] - " release_date
        next
    }
    { print }
' "$CHANGELOG" >"$tmp_changelog"

if [[ "$(grep -Ec "^## \\[${escaped_version}\\] - ${release_date}$" "$tmp_changelog")" -ne 1 ]]; then
  printf 'ERROR: generated changelog does not contain the exact release heading\n' >&2
  exit 1
fi

# The two `_comment_*` keys document the one-shot fields (ADR-1151); they go
# with them, otherwise the config keeps an explanation of a field it no longer
# has. release-please ignores unknown top-level keys, so their presence before
# the cut is inert.
jq 'del(."bootstrap-sha", ."_comment_bootstrap_sha",
        .packages["."]."release-as", .packages["."]."_comment_release_as")' \
  "$CONFIG" >"$tmp_config"
if ! jq -e '
  (has("bootstrap-sha") | not) and
  (has("_comment_bootstrap_sha") | not) and
  (.packages["."] | has("release-as") | not) and
  (.packages["."] | has("_comment_release_as") | not)
' "$tmp_config" >/dev/null; then
  printf 'ERROR: failed to retire one-time release-please fields\n' >&2
  exit 1
fi

mkdir -p "$FRAG_ROOT/releases"
source_commit="unknown"
if [[ -d "$REPO_ROOT/.git" || -f "$REPO_ROOT/.git" ]]; then
  source_commit="$(git -C "$REPO_ROOT" rev-parse HEAD)"
fi
receipt="$FRAG_ROOT/releases/$version.json"
if [[ -e "$receipt" ]]; then
  printf 'ERROR: rollover receipt already exists: %s\n' "$receipt" >&2
  exit 1
fi

mv "$tmp_changelog" "$CHANGELOG"
mv "$tmp_config" "$CONFIG"
printf '{\n  "version": "%s",\n  "date": "%s",\n  "source_commit": "%s",\n  "source_count": %d,\n  "rendered_sha256": "%s"\n}\n' \
  "$version" "$release_date" "$source_commit" "${#sources[@]}" "$body_sha256" >"$receipt"
rm -f -- "${sources[@]}"

"$CONCAT" --check
active_after=0
for section in added changed deprecated removed fixed security; do
  if [[ -d "$FRAG_ROOT/$section" ]]; then
    count="$(find "$FRAG_ROOT/$section" -maxdepth 1 -type f -name '*.md' ! -name '.*' | wc -l)"
    active_after=$((active_after + count))
  fi
done
if [[ "$active_after" -ne 0 ]]; then
  printf 'ERROR: active changelog fragments remain after rollover\n' >&2
  exit 1
fi
if [[ -f "$FRAG_ROOT/_pre_fragment_legacy.md" ]]; then
  printf 'ERROR: legacy Unreleased source remains after rollover\n' >&2
  exit 1
fi

printf 'Rolled %d changelog sources into [%s] (%s, sha256 %s).\n' \
  "${#sources[@]}" "$version" "$release_date" "$body_sha256"
