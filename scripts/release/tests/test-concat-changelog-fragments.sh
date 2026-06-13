#!/usr/bin/env bash
# Test harness for scripts/release/concat-changelog-fragments.sh.
# Covers D.2 fix: tempfile EXIT trap so tmp_body + tmp_out are cleaned up
# even when the --write path aborts early (awk pipeline failure, mv failure,
# or any set -e exit from within the --write branch).
#
# Usage: bash scripts/release/tests/test-concat-changelog-fragments.sh
#
# Exit 0 on all-pass, 1 on any failure.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONCAT_SCRIPT="$SCRIPT_DIR/../concat-changelog-fragments.sh"

if [[ ! -f "$CONCAT_SCRIPT" ]]; then
  printf 'ERROR: %s not found\n' "$CONCAT_SCRIPT" >&2
  exit 1
fi

pass=0
fail=0

check() {
  local desc="$1"
  local result="$2" # "pass" or "fail"
  if [[ "$result" == "pass" ]]; then
    printf 'PASS: %s\n' "$desc"
    pass=$((pass + 1))
  else
    printf 'FAIL: %s\n' "$desc" >&2
    fail=$((fail + 1))
  fi
}

printf '\n=== D.2 concat-changelog-fragments tempfile trap tests ===\n\n'

# ---------------------------------------------------------------------------
# T1: Verify the script contains the EXIT trap (static analysis)
# ---------------------------------------------------------------------------
if grep -q "trap 'rm -f.*tmp_body.*tmp_out.*' EXIT" "$CONCAT_SCRIPT"; then
  check "EXIT trap present in script source" pass
else
  check "EXIT trap present in script source" fail
fi

# ---------------------------------------------------------------------------
# T2: Verify the manual 'rm -f "$tmp_body"' line was removed (redundant after trap)
# ---------------------------------------------------------------------------
# The fix removes the post-mv manual cleanup; only the trap should remain.
# We check there is exactly ONE rm invocation mentioning tmp_body — the trap.
rm_count=$(grep -c 'rm -f.*tmp_body' "$CONCAT_SCRIPT" || true)
if [[ "$rm_count" -eq 1 ]]; then
  check "only one rm -f tmp_body reference (trap only, no duplicate)" pass
else
  check "only one rm -f tmp_body reference (trap only, no duplicate) — found $rm_count" fail
fi

# ---------------------------------------------------------------------------
# T3: Simulate awk failure — verify tempfiles are not left behind
# ---------------------------------------------------------------------------
# We create a minimal fake CHANGELOG.md + changelog.d structure, then force
# awk to fail by temporarily shadowing awk with a wrapper that exits 1.
# The script must exit non-zero AND leave no tmp_body/tmp_out in /tmp.

TMPDIR_D2="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_D2"' EXIT

# Build a minimal fake repo structure the script expects.
mkdir -p "$TMPDIR_D2/changelog.d/fixed"
cat >"$TMPDIR_D2/CHANGELOG.md" <<'EOF'
# Changelog

## [Unreleased]

### Fixed

- placeholder entry

## [3.0.0] - 2026-01-01

### Added

- initial release
EOF

cat >"$TMPDIR_D2/changelog.d/fixed/test-frag.md" <<'EOF'
- test fragment entry
EOF

# Record how many tmp files matching mktemp patterns exist before the run.
# We use a pattern that covers the default mktemp prefix on Linux.
before_count=$(find /tmp -maxdepth 1 -name 'tmp.*' -newer "$TMPDIR_D2" 2>/dev/null | wc -l)

# Shadow awk with a failing stub so the pipeline inside --write aborts.
fake_awk_dir="$(mktemp -d -p "$TMPDIR_D2")"
cat >"$fake_awk_dir/awk" <<'EOF'
#!/usr/bin/env bash
# Fail immediately to simulate an awk pipeline error.
exit 1
EOF
chmod +x "$fake_awk_dir/awk"

# Run the script with the faked awk; expect non-zero exit.
exit_code=0
PATH="$fake_awk_dir:$PATH" \
  bash "$CONCAT_SCRIPT" --write \
  2>/dev/null || exit_code=$?

# After the script exits (any code), count new tmp files.
after_count=$(find /tmp -maxdepth 1 -name 'tmp.*' -newer "$TMPDIR_D2" 2>/dev/null | wc -l)

if [[ "$exit_code" -ne 0 ]]; then
  check "script exits non-zero when awk fails" pass
else
  check "script exits non-zero when awk fails (exit code was 0)" fail
fi

if [[ "$after_count" -le "$before_count" ]]; then
  check "no new tmp files leaked after awk failure" pass
else
  leaked=$((after_count - before_count))
  check "no new tmp files leaked after awk failure (found $leaked new files)" fail
fi

# ---------------------------------------------------------------------------
# T4: Happy path — verify --write works correctly end-to-end
# ---------------------------------------------------------------------------
# Restore PATH (remove fake awk dir) and do a real --write run.
TMPDIR_HAPPY="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_HAPPY"' EXIT

mkdir -p "$TMPDIR_HAPPY/changelog.d/fixed"
cat >"$TMPDIR_HAPPY/CHANGELOG.md" <<'EOF'
# Changelog

## [Unreleased]

### Fixed

- old placeholder

## [3.0.0] - 2026-01-01

### Added

- initial release
EOF

cat >"$TMPDIR_HAPPY/changelog.d/fixed/d2-test.md" <<'EOF'
- D.2 test: concat-changelog-fragments tempfile trap
EOF

(
  cd "$TMPDIR_HAPPY"
  # Override REPO_ROOT by running from the fake dir; the script uses
  # SCRIPT_DIR to derive REPO_ROOT, so we must override those vars.
  # Easiest: copy the script locally and patch REPO_ROOT.
  patched_script="$TMPDIR_HAPPY/concat-patched.sh"
  sed "s|REPO_ROOT=.*|REPO_ROOT=\"$TMPDIR_HAPPY\"|" "$CONCAT_SCRIPT" >"$patched_script"
  bash "$patched_script" --write 2>/dev/null
)

if grep -q "D.2 test: concat-changelog-fragments tempfile trap" "$TMPDIR_HAPPY/CHANGELOG.md"; then
  check "--write happy path produces correct CHANGELOG.md output" pass
else
  check "--write happy path produces correct CHANGELOG.md output" fail
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
printf '\n=== Results: %d passed, %d failed ===\n' "$pass" "$fail"
if [[ "$fail" -gt 0 ]]; then
  exit 1
fi
