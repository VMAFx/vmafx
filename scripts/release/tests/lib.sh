# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Shared test harness library for release regression tests.
# Sourced by test-*.sh scripts in this directory.
#
# shellcheck shell=bash

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT INT TERM
pass=0
fail=0

ok() {
  printf 'PASS: %s\n' "$1"
  pass=$((pass + 1))
}

bad() {
  printf 'FAIL: %s\n' "$1" >&2
  fail=$((fail + 1))
}

check() {
  local description="$1"
  shift
  if "$@"; then
    ok "$description"
  else
    bad "$description"
  fi
}
