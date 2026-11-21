#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# require-private-ghcr-package.sh — exit 0 only when an organisation's GHCR
# container package is private (ADR-1564).
#
# The dev container (`ghcr.io/vidiko/vmafx-dev-mcp`, the `libvmaf-build` stage
# of dev/Containerfile) holds the full CUDA toolkit, Intel's oneAPI Base Kit
# and the ROCm payload. Their licences allow internal use; they do not allow
# the toolkit files to be redistributed (CUDA EULA 1.1.1 and Attachment A;
# Intel EULA for Developer Tools 2.1). Pushing a new version into a public
# package would distribute them, so dev-container-publish.yml runs this check
# before it pushes.
#
# Usage: require-private-ghcr-package.sh <org> <package>
#
# Reads GET /orgs/<org>/packages/container/<package> with `gh api` (GH_TOKEN:
# the publishing job's token, which reads a package whose Actions access
# grants the repository a role). Fails closed: an API error, an unknown
# package, an answer without a string `visibility`, or any visibility other
# than "private" exits 1. Wrong usage exits 2.
set -euo pipefail

readonly API_TIMEOUT_S=60

report() {
  printf '::error::%s\n' "$1" >&2
}

main() {
  if [[ $# -ne 2 || -z $1 || -z $2 ]]; then
    printf 'usage: %s <org> <package>\n' "${0##*/}" >&2
    return 2
  fi
  local org=$1 package=$2 answer visibility
  local name="ghcr.io/${org,,}/${package}"
  if ! answer=$(timeout "$API_TIMEOUT_S" gh api \
    -H "Accept: application/vnd.github+json" \
    "/orgs/${org}/packages/container/${package}"); then
    report "cannot read the visibility of ${name} (no such package, or the token cannot read it); refusing to push"
    return 1
  fi
  if ! visibility=$(jq -er '.visibility | strings' <<<"$answer" 2>/dev/null); then
    report "the package API answer for ${name} carries no visibility; refusing to push"
    return 1
  fi
  if [[ $visibility != "private" ]]; then
    report "${name} is ${visibility}, not private; its images hold toolkit files that may not be redistributed, so nothing is pushed"
    return 1
  fi
  printf '%s is private\n' "$name"
}

main "$@"
