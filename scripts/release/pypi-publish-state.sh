#!/usr/bin/env bash
# Decide whether a PyPI upload is still owed for one release version, and
# refuse any divergence from what the immutable index already holds.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Usage: pypi-publish-state.sh <pypi-project> <dist-name> <pep440-version> [dist-dir]
#
# <dist-dir> (default `dist`) must hold at least one `<dist-name>-<version>-*.whl`
# and exactly one `<dist-name>-<version>.tar.gz`. The script queries
# https://pypi.org/pypi/<pypi-project>/<version>/json and prints
# `publish=true|false` (appended to $GITHUB_OUTPUT when that is set):
#
#   HTTP 404               -> publish=true  (version not on PyPI yet)
#   HTTP 200, all present  -> publish=false (every local file already there,
#                                            same SHA-256)
#   HTTP 200, some missing -> publish=true  (upload the rest; the publish
#                                            action skips the existing ones)
#
# It exits 1 when PyPI holds a file this build did not produce, a file with a
# different SHA-256, or duplicate records, or answers with any other status:
# PyPI never lets a filename be re-used, so each of those is unrecoverable for
# this version and must stop the release instead of publishing a mixed set.

set -euo pipefail
shopt -s nullglob

if [[ $# -lt 3 || $# -gt 4 ]]; then
  echo "usage: $0 <pypi-project> <dist-name> <pep440-version> [dist-dir]" >&2
  exit 2
fi
project="$1" dist_name="$2" version="$3" dist_dir="${4:-dist}"

wheels=("$dist_dir/$dist_name-$version"-*.whl)
sdists=("$dist_dir/$dist_name-$version.tar.gz")
if [[ ${#wheels[@]} -lt 1 || ! -f "${sdists[0]}" ]]; then
  echo "ERROR: expected version-matched wheels and one sdist in $dist_dir" >&2
  exit 1
fi
local_files=("${wheels[@]}" "${sdists[0]}")

response="$(mktemp)"
trap 'rm -f "$response"' EXIT
http_code="$(curl --retry 3 --max-time 60 --silent --show-error \
  --output "$response" --write-out '%{http_code}' \
  "https://pypi.org/pypi/$project/$version/json")"

publish=false
case "$http_code" in
  200)
    while IFS= read -r filename; do
      if [[ ! -f "$dist_dir/$filename" ]]; then
        echo "ERROR: PyPI has unexpected file for $version: $filename" >&2
        exit 1
      fi
    done < <(jq -r '.urls[].filename' "$response")
    for f in "${local_files[@]}"; do
      filename="$(basename "$f")"
      local_digest="$(sha256sum -- "$f" | cut -d' ' -f1)"
      remote_count="$(jq --arg filename "$filename" \
        '[.urls[] | select(.filename == $filename)] | length' "$response")"
      case "$remote_count" in
        0) publish=true ;;
        1)
          remote_digest="$(jq -r --arg filename "$filename" \
            '.urls[] | select(.filename == $filename) | .digests.sha256' "$response")"
          if [[ "$remote_digest" != "$local_digest" ]]; then
            echo "ERROR: PyPI already has $filename with a different SHA-256" >&2
            exit 1
          fi
          ;;
        *)
          echo "ERROR: PyPI returned duplicate records for $filename" >&2
          exit 1
          ;;
      esac
    done
    ;;
  404) publish=true ;;
  *)
    echo "ERROR: PyPI release query returned HTTP $http_code" >&2
    exit 1
    ;;
esac

echo "publish=$publish"
if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
  echo "publish=$publish" >>"$GITHUB_OUTPUT"
fi
