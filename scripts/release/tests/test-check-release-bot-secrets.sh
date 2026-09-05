#!/usr/bin/env bash
# Tests for scripts/release/check-release-bot-secrets.sh (ADR-1171).
# A stub `gh` on PATH returns a configurable secret-name list, so the script's
# three exit codes are exercised without network or credentials.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
script="$here/../check-release-bot-secrets.sh"
stubdir="$(mktemp -d)"
trap 'rm -rf "$stubdir"' EXIT

make_stub() { # $1 = newline-separated names to print; $2 = exit code of gh
  cat >"$stubdir/gh" <<STUB
#!/usr/bin/env bash
if [[ "\$1" == "secret" && "\$2" == "list" ]]; then
    printf '%s\n' "$1"
    exit $2
fi
exit 99
STUB
  chmod +x "$stubdir/gh"
}

run_case() { # $1 = label, $2 = expected exit, then the stub args
  local label="$1" expected="$2"
  shift 2
  make_stub "$@"
  local rc=0
  PATH="$stubdir:$PATH" bash "$script" example/repo >/dev/null 2>&1 || rc=$?
  if [[ "$rc" -ne "$expected" ]]; then
    echo "FAIL $label: expected exit $expected, got $rc" >&2
    exit 1
  fi
  echo "ok   $label (exit $rc)"
}

run_case "both present" 0 $'RELEASE_BOT_APP_ID\nRELEASE_BOT_PRIVATE_KEY\nOTHER' 0
run_case "key missing" 1 $'RELEASE_BOT_APP_ID' 0
run_case "both missing" 1 $'OTHER' 0
run_case "no secrets at all" 1 '' 0
run_case "gh cannot list" 2 '' 1
echo "all check-release-bot-secrets cases passed"
