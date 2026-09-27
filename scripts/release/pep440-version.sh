#!/usr/bin/env bash
# Print the PEP 440 normalized form of a release version.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# The release tag, the manifest and every version marker carry the SemVer
# spelling (`1.0.0-rc.1`, ADR-1201). Python build backends do not: hatchling
# names the vmaf-mcp wheel and sdist after the PEP 440 normalized version
# (`vmaf_mcp-1.0.0rc1-py3-none-any.whl`, `vmaf_mcp-1.0.0rc1.tar.gz`), so a glob
# built from the SemVer string matches nothing for a release candidate.
#
# The conversion is deliberately exact for the two shapes the release verifier
# accepts and nothing else: `X.Y.Z` is already normalized, and `X.Y.Z-rc.N`
# becomes `X.Y.ZrcN`. Any other input -- a `v` prefix, a leading zero, a
# different prerelease label, a missing component -- fails closed instead of
# guessing, so a mis-shaped version can never produce a plausible filename.

set -euo pipefail
# Bracket ranges such as [0-9] match non-ASCII digits in some UTF-8 locales.
export LC_ALL=C

usage() {
  printf 'Usage: pep440-version.sh MAJOR.MINOR.PATCH[-rc.N]\n'
}

if [[ $# -ne 1 ]]; then
  usage >&2
  exit 64
fi

version="$1"
# Same shape as scripts/release/verify-release-version.sh, without the `v`.
if [[ ! "$version" =~ ^((0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*))(-rc\.(0|[1-9][0-9]*))?$ ]]; then
  printf 'ERROR: release version must be MAJOR.MINOR.PATCH or MAJOR.MINOR.PATCH-rc.N: %s\n' \
    "$version" >&2
  exit 64
fi

release="${BASH_REMATCH[1]}"
if [[ -n "${BASH_REMATCH[5]}" ]]; then
  printf '%src%s\n' "$release" "${BASH_REMATCH[6]}"
else
  printf '%s\n' "$release"
fi
