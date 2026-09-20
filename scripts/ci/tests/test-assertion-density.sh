#!/usr/bin/env bash
# Test harness for scripts/ci/assertion-density.sh.
# Covers ADR-1267's origin-neutral scope: fork, Netflix, and unlabelled tracked
# sources are all selected.
#
# Usage: bash scripts/ci/tests/test-assertion-density.sh
#
# Exit 0 on all-pass, 1 on any failure.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent

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
# run_test <desc> <match|skip|fail> <file>...
#   expect_match: the script must NOT exit 0 with "skipping" (it found files)
#   expect_skip:  the script must exit 0 with "skipping" in stdout
#                 (no matching files found)
#   fail:         the script must reject at least one long function without an
#                 assertion
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

    # Place fixtures under a conventional source path. The scanner selects every
    # tracked C-family source, so the directory and copyright text are irrelevant.
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
    local rc
    set +e
    output="$(cd "$repo" && bash "$DENSITY_SCRIPT" 2>&1)"
    rc=$?
    set -e

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
    fail)
        if [[ "$rc" -ne 0 ]] && echo "$output" | grep -q "0 asserts"; then
            printf 'PASS: %s — correctly rejected assertion-free function\n' "$desc"
            pass=$((pass + 1))
        else
            printf 'FAIL: %s — expected scanner failure but got rc=%d:\n%s\n' \
                "$desc" "$rc" "$output" >&2
            fail=$((fail + 1))
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

# Fixture: Netflix-only header — origin must not exempt it from the scan.
netflix_header_file="$TMPDIR_TESTS/netflix_header.c"
cat >"$netflix_header_file" <<'EOF'
// Copyright 2016-2024 Netflix, Inc.
// SPDX-License-Identifier: BSD-2-Clause-Patent

int netflix_func(int x) {
    return x + 1;
}
EOF

# Fixture: no copyright header at all — tracked source is still in scope.
no_header_file="$TMPDIR_TESTS/no_header.c"
cat >"$no_header_file" <<'EOF'
int bare_func(int x) {
    return x + 1;
}
EOF

# Fixture: storage-class prefixes must not hide a non-trivial function. This
# specifically catches the historical `static`/`inline` first-token blind spot.
static_long_file="$TMPDIR_TESTS/static_long.c"
cat >"$static_long_file" <<'EOF'
static int static_long_func(int x)
{
    x += 1;
    x += 2;
    x += 3;
    x += 4;
    x += 5;
    x += 6;
    x += 7;
    x += 8;
    x += 9;
    x += 10;
    x += 11;
    x += 12;
    x += 13;
    x += 14;
    x += 15;
    x += 16;
    x += 17;
    x += 18;
    x += 19;
    x += 20;
    return x;
}
EOF

# Fixtures: origin-neutral scope must be behavioral, not merely file selection.
netflix_long_file="$TMPDIR_TESTS/netflix_long.c"
{
    printf '%s\n' '// Copyright 2016-2024 Netflix, Inc.'
    sed 's/static_long_func/netflix_long_func/' "$static_long_file"
} >"$netflix_long_file"

bare_long_file="$TMPDIR_TESTS/bare_long.c"
sed 's/static_long_func/bare_long_func/' "$static_long_file" >"$bare_long_file"

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

printf '\n=== assertion-density origin-neutral scope tests ===\n\n'

# T1: legacy header must be matched.
run_test "legacy 'Lusoris and Claude' header" match "$legacy_header_file"

# T2: new rebrand header must be matched (this was the D.1 bug — pre-fix it
# would have been missed and the script would exit 0 with "skipping").
run_test "new 'Copyright 2026 Lusoris' header" match "$new_header_file"

# T3: Netflix-only file must be matched.
run_test "Netflix-only header is matched" match "$netflix_header_file"

# T4: a tracked source with no copyright header is still matched.
run_test "No-copyright-header file is matched" match "$no_header_file"

# T5: mix of legacy + new-format files — both must be picked up together.
run_test "mixed legacy + new headers both matched" match "$legacy_header_file" "$new_header_file"

# T6: mixed origins remain one project scope.
run_test "Lusoris + Netflix mix — both remain in scope" match "$new_header_file" "$netflix_header_file"

# T7: storage-class prefixes cannot hide a long assertion-free function.
run_test "static long function is enforced" fail "$static_long_file"

# T8: Netflix origin cannot hide a long assertion-free function.
run_test "Netflix long function is enforced" fail "$netflix_long_file"

# T9: missing copyright metadata cannot hide a long assertion-free function.
run_test "headerless long function is enforced" fail "$bare_long_file"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
printf '\n=== Results: %d passed, %d failed ===\n' "$pass" "$fail"
if [[ "$fail" -gt 0 ]]; then
    exit 1
fi
