#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# Fetch + sha256-verify the model/tiny/*.onnx blobs that are too large to
# inline in git history. Per ADR-0457, blobs >= 1 MB live as attachments
# on the `tiny-blobs-vN` GitHub Release rather than in the repo. The
# fetcher reads model/tiny/registry.json for the blob list, downloads each
# one that's missing locally, and verifies the recorded sha256.
#
# Idempotent: re-running with everything present is a no-op.
#
# Usage:
#   scripts/ai/fetch-tiny-blobs.sh                 # download missing blobs
#   scripts/ai/fetch-tiny-blobs.sh --check         # only verify present blobs
#   scripts/ai/fetch-tiny-blobs.sh --force         # re-download even if present
#
# Exit codes:
#   0  all blobs present and verified
#   1  download or verification failed
#   2  curl / jq / sha256sum missing
#   3  registry.json malformed

set -euo pipefail
IFS=$'\n\t'

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$REPO_ROOT/.." && pwd)"
REGISTRY="$REPO_ROOT/model/tiny/registry.json"

# Track in-flight mktemp staging files so a Ctrl-C or unexpected exit between
# mktemp and the success-path `mv` doesn't leave `*.XXXXXX` orphans next to
# `model/tiny/*.onnx`. The fetch_one helper appends to this array on
# allocation and clears it on successful mv.
_TINY_STAGING_FILES=()
_cleanup_staging() {
  if [ "${#_TINY_STAGING_FILES[@]}" -gt 0 ]; then
    rm -f "${_TINY_STAGING_FILES[@]}" 2>/dev/null || true
  fi
}
trap _cleanup_staging EXIT INT TERM

mode="fetch"
while [ $# -gt 0 ]; do
  case "$1" in
    --check)
      mode="check"
      shift
      ;;
    --force)
      mode="force"
      shift
      ;;
    -h | --help)
      sed -n '4,28p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "unknown arg: $1 (use --help)" >&2
      exit 2
      ;;
  esac
done

for tool in curl jq sha256sum; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: $tool not on PATH" >&2
    exit 2
  fi
done

if [ ! -f "$REGISTRY" ]; then
  echo "error: registry not found at $REGISTRY" >&2
  exit 3
fi

# Pull the list of (filename, sha256) tuples for blobs that are hosted as
# release attachments. Detection: presence of a non-empty "release_url"
# field flags an attachment-hosted blob. Inline (small) blobs are skipped.
mapfile -t entries < <(jq -r '
    .models[]
    | select(.release_url? != null and .release_url != "")
    | "\(.onnx)\t\(.sha256)\t\(.release_url)"
' "$REGISTRY")

if [ "${#entries[@]}" -eq 0 ]; then
  echo "no release-hosted blobs in registry; nothing to do."
  exit 0
fi

verify_one() {
  local file="$1"
  local expected_sha="$2"
  local actual_sha
  actual_sha="$(sha256sum "$file" | awk '{print $1}')"
  if [ "$actual_sha" = "$expected_sha" ]; then
    return 0
  fi
  echo "  ! sha256 mismatch for $file" >&2
  echo "    expected: $expected_sha" >&2
  echo "    actual:   $actual_sha" >&2
  return 1
}

fetch_one() {
  local url="$1"
  local dest="$2"
  local tmp
  tmp="$(mktemp "$dest.XXXXXX")"
  _TINY_STAGING_FILES+=("$tmp")
  if ! curl --fail --silent --show-error --location \
    --max-time 600 --retry 3 --retry-connrefused \
    --output "$tmp" "$url"; then
    rm -f "$tmp"
    # Pop the just-cleaned path so the EXIT trap doesn't double-rm.
    unset '_TINY_STAGING_FILES[${#_TINY_STAGING_FILES[@]}-1]'
    return 1
  fi
  mv "$tmp" "$dest"
  unset '_TINY_STAGING_FILES[${#_TINY_STAGING_FILES[@]}-1]'
}

failures=0
fetched=0
verified=0
skipped=0

for entry in "${entries[@]}"; do
  filename="$(echo "$entry" | cut -f1)"
  expected_sha="$(echo "$entry" | cut -f2)"
  release_url="$(echo "$entry" | cut -f3)"
  dest="$REPO_ROOT/model/tiny/$filename"

  if [ "$mode" = "force" ] || [ ! -f "$dest" ]; then
    if [ "$mode" = "check" ]; then
      echo "  ? $filename — missing (--check would re-download)"
      failures=$((failures + 1))
      continue
    fi
    echo "  > $filename — fetching from $release_url"
    if ! fetch_one "$release_url" "$dest"; then
      echo "  ! download failed for $filename" >&2
      failures=$((failures + 1))
      continue
    fi
    fetched=$((fetched + 1))
  else
    skipped=$((skipped + 1))
  fi

  if verify_one "$dest" "$expected_sha"; then
    verified=$((verified + 1))
  else
    failures=$((failures + 1))
  fi
done

echo "fetched=$fetched verified=$verified skipped=$skipped failures=$failures"
[ "$failures" -eq 0 ]
