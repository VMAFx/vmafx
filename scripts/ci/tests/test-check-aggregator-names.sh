#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Tests for scripts/ci/check-aggregator-names.sh: the required list must match
# the marked workflow names, and each required name has exactly one reporter.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
script="$here/../check-aggregator-names.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

expect() { # $1 label, $2 expected rc, $3 fixture root
  local rc=0
  bash "$script" "$3" >/dev/null 2>&1 || rc=$?
  if [ "$rc" -ne "$2" ]; then
    echo "FAIL $1: expected rc=$2 got rc=$rc" >&2
    exit 1
  fi
  echo "ok   $1 (rc=$rc)"
}

fixture() { # $1 root; stdin = second workflow
  mkdir -p "$1/.github/workflows"
  cat >"$1/.github/workflows/required-aggregator.yml" <<'YML'
jobs:
  aggregate:
    steps:
      - name: Evaluate
        with:
          script: |
            const required = [
              'Alpha Build',
              'Beta Lint',
            ];
YML
  cat >"$1/.github/workflows/matrix.yml" <<'YML'
name: Builds
jobs:
  build:
    name: ${{ matrix.name }}
    strategy:
      matrix:
        include:
          # required-aggregator
          - os: ubuntu-latest
            name: Alpha Build
    steps:
      - name: Alpha Build
        run: true
  lint:
    # required-aggregator
    name: Beta Lint
    steps:
    - name: Beta Lint
      run: true
YML
  cat >"$1/.github/workflows/other.yml"
}

fixture "$tmp/clean" <<'YML'
name: Beta Lint
jobs:
  extra:
    name: Gamma Extra
    steps:
      - name: Alpha Build
        run: true
YML
expect "one reporter per required name passes (step and workflow names ignored)" 0 "$tmp/clean"

fixture "$tmp/shared" <<'YML'
name: Other
jobs:
  full:
    name: ${{ matrix.name }}
    strategy:
      matrix:
        include:
          - os: windows-2025
            name: Alpha Build
YML
expect "a required name reported by a second workflow's job fails" 1 "$tmp/shared"

fixture "$tmp/unmarked" <<'YML'
name: Other
jobs:
  extra:
    # required-aggregator
    name: Delta Unlisted
YML
expect "a marked name missing from the required list fails" 1 "$tmp/unmarked"

echo "all check-aggregator-names tests passed"
