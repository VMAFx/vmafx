#!/usr/bin/env bash
# scripts/ci/test-sycl-bench-env.sh — shell-injection regression test
# for scripts/ci/sycl-bench-env.sh.
#
# Background: an earlier revision of sycl-bench-env.sh interpolated
# `$ROOT` directly into a `bash -c "…'$ROOT/setvars.sh'…"` command
# string. A `$ONEAPI_PREFIX` (env) or VERSION (CLI arg) containing a
# stray single-quote and shell metacharacters would have escaped the
# subshell and executed arbitrary code under the caller's UID. The
# refactor passes `$ROOT` as a positional argument to the subshell so
# its contents are treated as data regardless of shape.
#
# This test exercises the hardened path with a hostile `ONEAPI_PREFIX`
# that contains `'`, `;`, and `>&2` — if the old vulnerability ever
# regresses, "PWNED" appears on stderr and the test fails.
#
# Run from anywhere:
#   bash scripts/ci/test-sycl-bench-env.sh
#
# Exit codes:
#   0 — all cases pass
#   1 — at least one case failed
#   2 — preflight failure (gate script missing or non-executable)
#
# See changelog.d/security/shell-injection-sweep-round2.md.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GATE="${SCRIPT_DIR}/sycl-bench-env.sh"

if [ ! -f "$GATE" ]; then
  echo "test-sycl-bench-env: gate script not found: $GATE" >&2
  exit 2
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

pass=0
fail=0

ok() {
  echo "[test-sycl-bench-env] PASS: $1"
  pass=$((pass + 1))
}
ko() {
  echo "[test-sycl-bench-env] FAIL: $1" >&2
  fail=$((fail + 1))
}

# --- Case 1: clean prefix, prints export lines. -----------------------------
clean_root="${WORKDIR}/clean-prefix"
mkdir -p "$clean_root"
cat >"$clean_root/setvars.sh" <<'EOF'
#!/bin/sh
export CMPLR_ROOT=/safe/root
export LD_LIBRARY_PATH=/safe/lib
EOF
chmod +x "$clean_root/setvars.sh"
out1="$(ONEAPI_PREFIX="$clean_root" bash "$GATE" latest --quiet 2>/dev/null || true)"
if echo "$out1" | grep -q 'export CMPLR_ROOT=/safe/root'; then
  ok "clean prefix emits export lines"
else
  ko "clean prefix did not emit expected export"
fi

# --- Case 2: hostile prefix with quote + OR-chain cannot escape subshell. -
# Original vulnerable shape: bash -c "... source '$ROOT/setvars.sh' ...".
# An OR-fallback payload (close the single-quote literal early, append
# `|| touch <marker>`, then `false #` to swallow the trailing `' …`) runs
# inside the subshell when set -e cannot block the OR-branch.
#
# The CVE-shaped vector that the positional-arg refactor blocks: $ROOT is
# now $1 inside the subshell, never re-tokenised by the shell parser. A
# side-channel marker file is the oracle here — string-matching on stderr
# is unreliable because the script's own "error: …" line echoes the
# hostile path verbatim.
#
# The gate's predicate `[ ! -f "$ROOT/setvars.sh" ]` exits early if the
# constructed path doesn't resolve to a real file, so we create a real
# directory whose literal name *is* the injection payload and drop a
# real setvars.sh inside. Only then does control reach `bash -c`.
hostile_root="${WORKDIR}/missing' || touch ${WORKDIR}/pwn-or-marker; false # "
mkdir -p "$hostile_root"
cat >"$hostile_root/setvars.sh" <<'EOF'
#!/bin/sh
export CMPLR_ROOT=/contained
EOF
chmod +x "$hostile_root/setvars.sh"
ONEAPI_PREFIX="$hostile_root" bash "$GATE" latest --quiet >/dev/null 2>&1 || true
if [ -f "${WORKDIR}/pwn-or-marker" ]; then
  ko "hostile ONEAPI_PREFIX || escaped subshell (marker file created)"
  rm -f "${WORKDIR}/pwn-or-marker"
else
  ok "hostile ONEAPI_PREFIX || contained (no marker file)"
fi

# --- Case 3: $(...) substitution inside single-quote literal is inert. ---
# Even the original (vulnerable) form contained `$(…)` injections because
# the substitution sits inside a single-quoted literal in the bash -c body.
# This case is a positive smoke check — both old and new forms must keep
# command-substitution payloads inert.
hostile2_root="${WORKDIR}/dir\$(touch ${WORKDIR}/pwn-sub-marker)"
mkdir -p "$hostile2_root"
cat >"$hostile2_root/setvars.sh" <<'EOF'
#!/bin/sh
export CMPLR_ROOT=/contained2
EOF
chmod +x "$hostile2_root/setvars.sh"
ONEAPI_PREFIX="$hostile2_root" bash "$GATE" latest --quiet >/dev/null 2>&1 || true
if [ -f "${WORKDIR}/pwn-sub-marker" ]; then
  ko "hostile ONEAPI_PREFIX \$(…) escaped subshell (marker file created)"
  rm -f "${WORKDIR}/pwn-sub-marker"
else
  ok "hostile ONEAPI_PREFIX \$(…) contained (no marker file)"
fi

# --- Case 4: missing version arg exits non-zero. --------------------------
if bash "$GATE" >/dev/null 2>&1; then
  ko "missing version did not exit non-zero"
else
  ok "missing version exits non-zero"
fi

echo "[test-sycl-bench-env] summary: ${pass} pass, ${fail} fail"
if [ "$fail" -gt 0 ]; then
  exit 1
fi
exit 0
