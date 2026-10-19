#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Tests for scripts/ci/check-vcs-version-not-bare-sha.sh: a throwaway repo holds
# the real core/include/meson.build and the three fixture tester workflows. Positive:
# the contract form passes (with and without --long). Negative: --always, a
# describe without --match, a moved describe and a missing workflow fail.
# Boundary: comments that mention the banned flag are ignored.
set -euo pipefail

# Drop the git hook environment and the fixture identity (see the helper).
# shellcheck source=/dev/null
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/../../lib/clean-git-env.sh"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
git -C "$tmp" init -q
mkdir -p "$tmp/core/include" "$tmp/.github/workflows" "$tmp/scripts/ci"
cp "$root/core/include/meson.build" "$tmp/core/include/meson.build"
cp "$root/scripts/ci/check-vcs-version-not-bare-sha.sh" "$tmp/scripts/ci/"
macos="$tmp/.github/workflows/macos-tester-bundle.yml"
docker="$tmp/.github/workflows/docker-publish-tester.yml"
windows="$tmp/.github/workflows/windows-tester-bundle.yml"

# wf FILE TEXT: write one workflow line (TEXT is literal, never expanded).
wf() { printf '          %s\n' "$2" >"$1"; }
M="--match 'v*.*.*'"
expect() { # $1 label, $2 expected rc
  local rc=0
  (cd "$tmp" && bash scripts/ci/check-vcs-version-not-bare-sha.sh >/dev/null 2>&1) || rc=$?
  if [ "$rc" -ne "$2" ]; then
    echo "FAIL $1: expected rc=$2 got rc=$rc" >&2
    exit 1
  fi
  echo "ok   $1 (rc=$rc)"
}

wf "$macos" "d=\$(git describe --tags $M \$sha)"
wf "$docker" "d=\$(git describe --tags $M \$sha)"
wf "$windows" "d=\$(git describe --tags $M \$sha)"
expect "all three workflows on the contract form pass" 0

wf "$docker" "d=\$(git describe --tags --long $M \$sha)"
expect "--long with --match passes" 0

wf "$macos" "# git describe --tags --always is banned here"
wf "$macos.2" "d=\$(git describe --tags $M \$sha)"
cat "$macos.2" >>"$macos"
rm "$macos.2"
expect "a comment naming --always is ignored" 0

wf "$macos" "d=\$(git describe --tags $M \$sha)"
wf "$docker" "d=\$(git describe --tags --always \$sha)"
expect "--always without --match in docker workflow fails" 1

wf "$docker" "d=\$(git describe --tags $M \$sha)"
wf "$macos" "d=\$(git describe --tags --always $M \$sha)"
expect "--always with --match in macos workflow fails" 1

wf "$macos" "d=\$(git describe --tags \$sha)"
expect "describe without --match fails" 1

wf "$macos" "d=\$(compute_version \$sha)"
expect "a workflow with no git describe fails" 1

wf "$macos" "d=\$(git describe --tags $M \$sha)"
wf "$windows" "d=\$(git describe --tags --always $M \$sha)"
expect "--always in the windows workflow fails" 1

wf "$windows" "d=\$(git describe --tags \$sha)"
expect "describe without --match in the windows workflow fails" 1

wf "$windows" "d=\$(git describe --tags $M \$sha)"

wf "$macos" "d=\$(git describe --tags $M \$sha)"
rm "$docker"
expect "a missing workflow fails" 1

wf "$docker" "d=\$(git describe --tags $M \$sha)"
rm "$windows"
expect "a missing windows workflow fails" 1
