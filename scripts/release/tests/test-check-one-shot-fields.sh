#!/usr/bin/env bash
# Regression test for check-one-shot-fields.sh.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Runs the gate against fixture repositories (manifest + config) and holds each
# verdict: the 0.x restart one-shot passes on the candidate manifest, a
# release-as that survives into 0.x or later fails, a candidate manifest
# without prerelease versioning or release-as fails, and an unreadable manifest
# fails closed. It also checks that rule-enforcement.yml calls the script
# rather than an inline copy. CHECK_ONE_SHOT_SCRIPT runs another
# implementation against the same cases.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../../.." && pwd)"
CHECK="${CHECK_ONE_SHOT_SCRIPT:-$REPO_ROOT/scripts/release/check-one-shot-fields.sh}"
WORKFLOW="$REPO_ROOT/.github/workflows/rule-enforcement.yml"
# shellcheck source=scripts/release/tests/lib.sh
. "$SCRIPT_DIR/lib.sh"

# fixture NAME MANIFEST_JSON CONFIG_JSON
fixture() {
  mkdir -p "$scratch/$1"
  printf '%s\n' "$2" >"$scratch/$1/.release-please-manifest.json"
  printf '%s\n' "$3" >"$scratch/$1/release-please-config.json"
}

# expect NAME pass|fail DESCRIPTION
expect() {
  local status=0
  VMAFX_REPO_ROOT="$scratch/$1" bash "$CHECK" >"$scratch/$1.log" 2>&1 || status=$?
  if [[ "$2" == pass && "$status" -eq 0 ]] || [[ "$2" == fail && "$status" -ne 0 ]]; then
    ok "$3"
  else
    bad "$3 (exit $status, expected $2)"
    sed 's/^/    /' "$scratch/$1.log" >&2
  fi
}

restart_cfg='{"packages":{".":{"versioning":"default","prerelease":false,"release-as":"0.1.0"}}}'
default_cfg='{"packages":{".":{"versioning":"default","prerelease":false}}}'
prerelease_cfg='{"packages":{".":{"versioning":"prerelease","prerelease":true}}}'
bootstrap_cfg='{"bootstrap-sha":"0123abcd","packages":{".":{"versioning":"default"}}}'

fixture restart '{".":"1.0.0-rc.3"}' "$restart_cfg"
fixture rc-default '{".":"1.0.0-rc.3"}' "$default_cfg"
fixture rc-prerelease '{".":"1.0.0-rc.3"}' "$prerelease_cfg"
fixture zero-survivor '{".":"0.1.0"}' "$restart_cfg"
fixture zero-clean '{".":"0.1.0"}' "$default_cfg"
fixture zero-bootstrap '{".":"0.2.0"}' "$bootstrap_cfg"
fixture stable-bootstrap '{".":"1.2.0"}' "$bootstrap_cfg"
fixture stable-clean '{".":"1.2.0"}' "$default_cfg"
fixture no-root '{"core":"0.1.0"}' "$default_cfg"

expect restart pass "candidate manifest + default versioning + release-as (0.x restart)"
expect rc-default fail "candidate manifest + default versioning without release-as"
expect rc-prerelease pass "candidate manifest + prerelease versioning"
expect zero-survivor fail "release-as survives at manifest 0.1.0"
expect zero-clean pass "manifest 0.1.0 without one-shot fields"
expect zero-bootstrap fail "bootstrap-sha survives at manifest 0.2.0"
expect stable-bootstrap fail "bootstrap-sha survives at manifest 1.2.0"
expect stable-clean pass "manifest 1.2.0 without one-shot fields"
expect no-root fail "manifest without a root package fails closed"

if VMAFX_REPO_ROOT="$REPO_ROOT" bash "$CHECK" >"$scratch/repo.log" 2>&1; then
  ok "the repository's own manifest and config pass"
else
  bad "the repository's own manifest and config fail"
  sed 's/^/    /' "$scratch/repo.log" >&2
fi

if grep -Eq '^ +run: bash scripts/release/check-one-shot-fields\.sh$' "$WORKFLOW" &&
  ! grep -q 'survivors=' "$WORKFLOW"; then
  ok "rule-enforcement.yml calls the script and holds no inline copy"
else
  bad "rule-enforcement.yml does not call scripts/release/check-one-shot-fields.sh, or keeps an inline copy"
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[[ "$fail" -eq 0 ]]
