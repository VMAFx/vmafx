#!/usr/bin/env bash
# Test harness for scripts/ci/assertion-density.sh.
# Covers D.1 fix: rebrand-proof copyright grep accepting both
#   "Lusoris and Claude (Anthropic)" (legacy) and
#   "Copyright YYYY Lusoris" (current, post-2026-05-27 rebrand).
#
# Usage: bash scripts/ci/tests/test-assertion-density.sh
#
# Exit 0 on all-pass, 1 on any failure.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

set -euo pipefail

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
# run_test <desc> <expect_match|expect_skip> <file>...
#   expect_match: the script must NOT exit 0 with "skipping" (it found files)
#   expect_skip:  the script must exit 0 with "skipping" in stdout
#                 (no matching files found)
run_test() {
  local desc="$1"
  local mode="$2" # "match" or "skip"
  shift 2
  local files=("$@")

  # Build a throwaway git repo containing all the fixture files.
  local repo
  repo="$(mktemp -d -p "$TMPDIR_TESTS")"
  git -C "$repo" init -q
  git -C "$repo" config user.email "test@example.com"
  git -C "$repo" config user.name "Test"

  # Place files under core/src/feature/ so they match the glob in
  # assertion-density.sh (the script uses 'core/src/**/*.c', which requires
  # at least one subdirectory level under core/src/).
  mkdir -p "$repo/core/src/feature"
  local relpath
  local i=0
  for src_path in "${files[@]}"; do
    relpath="core/src/feature/fixture_${i}.c"
    cp "$src_path" "$repo/$relpath"
    git -C "$repo" add "$relpath"
    i=$((i + 1))
  done
  git -C "$repo" commit -q -m "init"

  # Run the script from inside the fake repo.
  local output
  output="$(cd "$repo" && bash "$DENSITY_SCRIPT" 2>&1)" || true

  case "$mode" in
    skip)
      if echo "$output" | grep -q "skipping"; then
        printf 'PASS: %s — correctly skipped (no matching copyright)\n' "$desc"
        pass=$((pass + 1))
      else
        printf 'FAIL: %s — expected "skipping" but got:\n%s\n' "$desc" "$output" >&2
        fail=$((fail + 1))
      fi
      ;;
    match)
      if echo "$output" | grep -q "skipping"; then
        printf 'FAIL: %s — script skipped but should have matched files:\n%s\n' "$desc" "$output" >&2
        fail=$((fail + 1))
      else
        printf 'PASS: %s — correctly matched files, output:\n  %s\n' "$desc" "$(echo "$output" | head -3 | tr '\n' '|')"
        pass=$((pass + 1))
      fi
      ;;
    *)
      printf 'ERROR: unknown mode %s\n' "$mode" >&2
      exit 1
      ;;
  esac
}

# ---------------------------------------------------------------------------
# Fixture files
# ---------------------------------------------------------------------------

# Fixture: legacy header "Lusoris and Claude (Anthropic)" + one short function
# (below MIN_LINES threshold) with an assert — should be picked up and PASS.
legacy_header_file="$TMPDIR_TESTS/legacy_header.c"
cat >"$legacy_header_file" <<'EOF'
// Copyright 2025 Lusoris and Claude (Anthropic)
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

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
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

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
// SPDX-License-Identifier: BSD+Patent

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

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
printf '\n=== Results: %d passed, %d failed ===\n' "$pass" "$fail"
if [[ "$fail" -gt 0 ]]; then
  exit 1
fi
