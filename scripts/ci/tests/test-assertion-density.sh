#!/usr/bin/env bash
# Test harness for scripts/ci/assertion-density.sh.
# Covers D.1: rebrand-proof copyright grep accepting both
#   "Lusoris and Claude (Anthropic)" (legacy) and
#   "Copyright YYYY Lusoris" (current, post-2026-05-27 rebrand),
# and issue #3347: the gate scans every non-mirror C source by default
# (the header grep is gone), while inherited and third-party files are
# exempted by exact path through scripts/ci/assertion-density-exempt-paths.txt.
# Each fixture repo carries the real scripts/ci/pelorus_mirror.py filter the
# gate pipes its source list through (plus the mirror and exemption
# manifests); a broken filter must fail the gate.
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
# run_test <desc> <match|skip|finding|error> <file>...
#   match:   the script exits 0 or 1 without "skipping" (it found files)
#   skip:    the script exits 0 with "skipping" in stdout (no scanned files)
#   finding: the script exits 1 with a planted assert-less function reported
#   error:   the script exits 2 (its source listing failed); FILTER=broken
#            puts a mirror filter that exits 3 into the fixture repo
# make_repo <dir> <file>... — a committed fixture repo holding the files under
# core/src/feature/ (the gate globs 'core/src/**/*.c'), or directly under
# core/src/ when TOPLEVEL=1, the gate's mirror filter, and an exemption
# manifest holding $EXEMPT_LIST (paths the gate must exempt; empty by
# default). FILTER=broken replaces the filter with one that exits 3.
make_repo() {
  local repo="$1" relpath i=0
  shift
  git -C "$repo" init -q
  mkdir -p "$repo/core/src/feature" "$repo/scripts/ci"
  for src_path in "$@"; do
    if [[ "${TOPLEVEL:-0}" == 1 ]]; then
      relpath="core/src/fixture_${i}.c"
    else
      relpath="core/src/feature/fixture_${i}.c"
    fi
    cp "$src_path" "$repo/$relpath"
    git -C "$repo" add "$relpath"
    i=$((i + 1))
  done
  git -C "$repo" commit -q -m "init"
  cp "$SCRIPT_DIR/../pelorus_mirror.py" "$SCRIPT_DIR/../pelorus-mirror-paths.txt" "$repo/scripts/ci/"
  printf '%s\n' "${EXEMPT_LIST:-}" >"$repo/scripts/ci/assertion-density-exempt-paths.txt"
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
    finding) [[ "$rc" -eq 1 && "$skipped" -eq 0 ]] ;;
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

# Helper fixture body: a function over the MIN_LINES threshold with no assert.
# (Both the contributor-named, the bare and the inherited fixtures plant it, so
# the gate's verdict on them reflects the scope rule, not function length.)
long_assertless_body='int planted_without_assert(int x) {
    int y = x;
    y += 1;
    y *= 2;
    y -= 3;
    y += 4;
    y *= 5;
    y -= 6;
    y += 7;
    y *= 8;
    y -= 9;
    y += 10;
    y *= 11;
    y -= 12;
    y += 13;
    y *= 14;
    y -= 15;
    y += 16;
    y *= 17;
    y -= 18;
    y += 19;
    y *= 20;
    return y;
}'

# Fixture: an outside contributor's own copyright line (ADR-3401 says a file a
# contributor writes carries their name, not Lusoris) + an assert-less function
# over the threshold. Issue #3347: the old marker filter skipped this file and
# the gate passed without scanning it.
contributor_header_file="$TMPDIR_TESTS/contributor_header.c"
cat >"$contributor_header_file" <<EOF
// Copyright 2026 Ada Lovelace
// SPDX-License-Identifier: BSD-2-Clause-Patent

$long_assertless_body
EOF

# Fixture: no copyright header at all — still scanned by default (the gate
# decides fork ownership by the mirror filter, not by a header name).
bare_header_file="$TMPDIR_TESTS/bare_header.c"
cat >"$bare_header_file" <<EOF
$long_assertless_body
EOF

# Fixture: Netflix-only header — inherited code. Without an exemption entry it
# is scanned like any other non-mirror candidate (and its planted assert-less
# function is a finding); with the exact-path entry the gate exempts it.
netflix_header_file="$TMPDIR_TESTS/netflix_header.c"
cat >"$netflix_header_file" <<EOF
// Copyright 2016-2024 Netflix, Inc.
// SPDX-License-Identifier: BSD-2-Clause-Patent

$long_assertless_body
EOF

# Fixture: a file directly under core/src/ with a function over the line
# threshold and no assert. The old pathspec ('core/src/**/*.c' without
# :(glob)) never listed it, so the gate read as "skipping" and passed.
toplevel_no_assert_file="$TMPDIR_TESTS/toplevel_no_assert.c"
cat >"$toplevel_no_assert_file" <<'EOF'
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent

int top_level_without_assert(int x) {
    int y = x;
    y += 1;
    y *= 2;
    y -= 3;
    y += 4;
    y *= 5;
    y -= 6;
    y += 7;
    y *= 8;
    y -= 9;
    y += 10;
    y *= 11;
    y -= 12;
    y += 13;
    y *= 14;
    y -= 15;
    y += 16;
    y *= 17;
    y -= 18;
    y += 19;
    y *= 20;
    return y;
}
EOF

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

printf '\n=== assertion-density scope tests (rebrand grep, scan-by-default, exemptions) ===\n\n'

# T1: legacy Lusoris header must be scanned.
run_test "legacy 'Lusoris and Claude' header" match "$legacy_header_file"

# T2: new rebrand header must be scanned (this was the D.1 bug — pre-fix it
# would have been missed and the script would exit 0 with "skipping").
run_test "new 'Copyright 2026 Lusoris' header" match "$new_header_file"

# T3 (issue #3347): a contributor-named file with an assert-less function over
# the threshold is a finding. The old marker filter skipped it and the gate
# passed without scanning the file.
run_test "contributor-named assert-less file is scanned and fails the gate" \
  finding "$contributor_header_file"

# T4 (issue #3347): a file with no copyright line at all is scanned by default —
# fork ownership comes from the mirror filter, not a header name.
run_test "no-copyright-header file is scanned (assert-less function is a finding)" \
  finding "$bare_header_file"

# T5: mix of legacy + new-format files — both must be picked up together.
run_test "mixed legacy + new headers both matched" match "$legacy_header_file" "$new_header_file"

# T6 (issue #3347): an inherited Netflix-header file with no exemption entry is
# scanned like any other non-mirror candidate, so its planted assert-less
# function is a finding.
run_test "inherited Netflix-header file without an exemption entry is scanned" \
  finding "$new_header_file" "$netflix_header_file"

# T7 (issue #3347): the same file with its exact path in the exemption manifest
# is exempt — the gate finds nothing left to scan and skips.
EXEMPT_LIST="core/src/feature/fixture_0.c" \
  run_test "inherited Netflix-header file exempted by exact path" \
  skip "$netflix_header_file"

# T8 (issue #3347): an exemption entry the source listing no longer finds
# (removed, renamed or moved out of the pathspecs) fails the gate, so the
# manifest cannot go stale silently.
EXEMPT_LIST="core/src/feature/gone.c" \
  run_test "stale exemption entry fails the gate" error "$netflix_header_file"

# T9: a mirror filter that fails must fail the gate, not read as "no files".
FILTER=broken run_test "a failing mirror filter fails the gate" error "$new_header_file"

# T10: a file directly under core/src/ is in scope. With the old pathspec the
# gate found no files and skipped; with ':(glob)' it must report the function.
TOPLEVEL=1 run_test "an assert-less function directly under core/src/ is a finding" \
  finding "$toplevel_no_assert_file"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
printf '\n=== Results: %d passed, %d failed ===\n' "$pass" "$fail"
if [[ "$fail" -gt 0 ]]; then
  exit 1
fi
