#!/usr/bin/env bash
# Test harness for scripts/ci/assertion-density.sh.
# Covers D.1 fix: rebrand-proof copyright grep accepting both
#   "Lusoris and Claude (Anthropic)" (legacy) and
#   "Copyright YYYY Lusoris" (current, post-2026-05-27 rebrand).
# Each fixture repo carries the real scripts/ci/pelorus_mirror.py filter the
# gate pipes its source list through; a broken filter must fail the gate.
#
# Usage: bash scripts/ci/tests/test-assertion-density.sh
#
# Exit 0 on all-pass, 1 on any failure.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent

set -euo pipefail

# Drop the git hook environment and the fixture identity (see the helper).
# shellcheck source=/dev/null
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/../../lib/clean-git-env.sh"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
DENSITY_SCRIPT="$SCRIPT_DIR/../assertion-density.sh"

if [[ ! -f "$DENSITY_SCRIPT" ]]; then
  printf 'ERROR: %s not found\n' "$DENSITY_SCRIPT" >&2
  exit 1
fi

TMPDIR_TESTS="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_TESTS"' EXIT

pass=0
fail=0

# Helper: run assertion-density.sh against a fake git repo containing
# the specified files, then check stdout for a substring.
#
# run_test <desc> <match|skip|error> <file>...
#   match: the script exits 0 or 1 without "skipping" (it found files)
#   skip:  the script exits 0 with "skipping" in stdout (no matching files)
#   error: the script exits 2 (its source listing failed); FILTER=broken
#          puts a mirror filter that exits 3 into the fixture repo
# make_repo <dir> <file>... — a committed fixture repo holding the files under
# core/src/feature/ (the gate globs 'core/src/**/*.c', which needs one
# subdirectory level under core/src/) and the gate's mirror filter;
# FILTER=broken replaces the filter with one that exits 3.
make_repo() {
  local repo="$1" relpath i=0
  shift
  git -C "$repo" init -q
  mkdir -p "$repo/core/src/feature" "$repo/scripts/ci"
  for src_path in "$@"; do
    relpath="core/src/feature/fixture_${i}.c"
    cp "$src_path" "$repo/$relpath"
    git -C "$repo" add "$relpath"
    i=$((i + 1))
  done
  git -C "$repo" commit -q -m "init"
  cp "$SCRIPT_DIR/../pelorus_mirror.py" "$SCRIPT_DIR/../pelorus-mirror-paths.txt" "$repo/scripts/ci/"
  if [[ "${FILTER:-real}" == broken ]]; then
    printf '%s\n' 'import sys' 'sys.exit(3)' >"$repo/scripts/ci/pelorus_mirror.py"
  fi
}

# verdict <mode> <rc> <output> — succeeds when the gate behaved as <mode> says.
verdict() {
  local mode="$1" rc="$2" output="$3" skipped=0
  if echo "$output" | grep -q "skipping"; then skipped=1; fi
  case "$mode" in
    error) [[ "$rc" -eq 2 && "$skipped" -eq 0 ]] ;;
    skip) [[ "$rc" -eq 0 && "$skipped" -eq 1 ]] ;;
    match) [[ "$rc" -le 1 && "$skipped" -eq 0 ]] ;;
    *)
      printf 'ERROR: unknown mode %s\n' "$mode" >&2
      exit 1
      ;;
  esac
}

run_test() {
  local desc="$1" mode="$2" repo output rc=0
  shift 2
  repo="$(mktemp -d -p "$TMPDIR_TESTS")"
  make_repo "$repo" "$@"
  output="$(cd "$repo" && bash "$DENSITY_SCRIPT" 2>&1)" || rc=$?
  if verdict "$mode" "$rc" "$output"; then
    printf 'PASS: %s (%s, rc=%d): %s\n' "$desc" "$mode" "$rc" "$(echo "$output" | head -3 | tr '\n' '|')"
    pass=$((pass + 1))
  else
    printf 'FAIL: %s — expected %s, got rc=%d:\n%s\n' "$desc" "$mode" "$rc" "$output" >&2
    fail=$((fail + 1))
  fi
}

# ---------------------------------------------------------------------------
# Fixture files
# ---------------------------------------------------------------------------

# Fixture: legacy header "Lusoris and Claude (Anthropic)" + one short function
# (below MIN_LINES threshold) with an assert — should be picked up and PASS.
legacy_header_file="$TMPDIR_TESTS/legacy_header.c"
cat >"$legacy_header_file" <<'EOF'
// Copyright 2025 Lusoris and Claude (Anthropic)
// SPDX-License-Identifier: BSD-2-Clause-Patent

#include <assert.h>

int legacy_func(int x) {
    assert(x >= 0);
    return x + 1;
}
EOF

# Fixture: new "Copyright YYYY Lusoris" header + one short function with assert.
new_header_file="$TMPDIR_TESTS/new_header.c"
cat >"$new_header_file" <<'EOF'
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent

#include <assert.h>

int new_func(int x) {
    assert(x >= 0);
    return x + 1;
}
EOF

# Fixture: Netflix-only header — must NOT be picked up by the scan.
netflix_header_file="$TMPDIR_TESTS/netflix_header.c"
cat >"$netflix_header_file" <<'EOF'
// Copyright 2016-2024 Netflix, Inc.
// SPDX-License-Identifier: BSD-2-Clause-Patent

int netflix_func(int x) {
    return x + 1;
}
EOF

# Fixture: no copyright header at all — must NOT be picked up.
no_header_file="$TMPDIR_TESTS/no_header.c"
cat >"$no_header_file" <<'EOF'
int bare_func(int x) {
    return x + 1;
}
EOF

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

printf '\n=== D.1 assertion-density copyright-grep rebrand tests ===\n\n'

# T1: legacy header must be matched.
run_test "legacy 'Lusoris and Claude' header" match "$legacy_header_file"

# T2: new rebrand header must be matched (this was the D.1 bug — pre-fix it
# would have been missed and the script would exit 0 with "skipping").
run_test "new 'Copyright 2026 Lusoris' header" match "$new_header_file"

# T3: Netflix-only file must NOT be matched — script should skip.
run_test "Netflix-only header is skipped" skip "$netflix_header_file"

# T4: no-header file must NOT be matched.
run_test "No-copyright-header file is skipped" skip "$no_header_file"

# T5: mix of legacy + new-format files — both must be picked up together.
run_test "mixed legacy + new headers both matched" match "$legacy_header_file" "$new_header_file"

# T6: mix of Lusoris + Netflix — only Lusoris files picked up (Netflix excluded).
# A repo with only Lusoris + Netflix: the script finds the Lusoris file and runs.
run_test "Lusoris + Netflix mix — Lusoris file found" match "$new_header_file" "$netflix_header_file"

# T7: a mirror filter that fails must fail the gate, not read as "no files".
FILTER=broken run_test "a failing mirror filter fails the gate" error "$new_header_file"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
printf '\n=== Results: %d passed, %d failed ===\n' "$pass" "$fail"
if [[ "$fail" -gt 0 ]]; then
  exit 1
fi
