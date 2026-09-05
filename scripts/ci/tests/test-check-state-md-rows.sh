#!/usr/bin/env bash
# Tests for scripts/ci/check-state-md-rows.sh (ADR-0165 row hygiene).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
script="$here/../check-state-md-rows.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

expect() { # $1 label, $2 expected rc, $3 file
  local rc=0
  bash "$script" "$3" >/dev/null 2>&1 || rc=$?
  if [ "$rc" -ne "$2" ]; then
    echo "FAIL $1: expected rc=$2 got rc=$rc" >&2
    exit 1
  fi
  echo "ok   $1 (rc=$rc)"
}

cat >"$tmp/clean.md" <<'MD'
## Open bugs

| ID | Description |
| --- | --- |
| **T-ALPHA-2026-01-01** | still broken |

## Recently closed

| ID | Description |
| --- | --- |
| **T-BETA-2026-01-02** | fixed |
MD
expect "a file with unique ids passes" 0 "$tmp/clean.md"

# the keep-both artefact: the same id under Open and under Recently closed
sed 's/T-BETA-2026-01-02/T-ALPHA-2026-01-01/' "$tmp/clean.md" >"$tmp/cross.md"
expect "an id in two sections fails" 1 "$tmp/cross.md"

# the same id twice inside one section
{
  cat "$tmp/clean.md"
  printf '| **T-BETA-2026-01-02** | fixed again |\n'
} >"$tmp/same.md"
expect "an id twice in one section fails" 1 "$tmp/same.md"

expect "a missing file is rc=2" 2 "$tmp/nope.md"

# the real file must be clean
expect "docs/state.md is clean" 0 "$here/../../../docs/state.md"
echo "all check-state-md-rows cases passed"
