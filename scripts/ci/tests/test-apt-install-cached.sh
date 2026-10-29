#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Tests for scripts/ci/apt-install-cached.sh against a stub sudo: the package
# list, apt arguments, retry config and archive cache round trip must reach
# apt, and bad usage, a missing list or an apt failure must fail the step.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
script="$here/../apt-install-cached.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
pass=0
fail=0

mkdir -p "$tmp/bin"
cat >"$tmp/bin/sudo" <<'STUB'
#!/usr/bin/env bash
echo "sudo $*" >>"$STUB_LOG"
case "$1" in
tee) cat >>"$STUB_LOG" ;;
apt-get)
  if [ "$2" = install ] && [ -n "${STUB_APT_FAIL:-}" ]; then exit 100; fi
  if [ "$2" = install ]; then : >"$APT_ARCHIVE_DIR/fresh.deb"; fi
  ;;
cp) shift; cp "$@" ;;
chown) ;;
*) echo "stub sudo: unexpected $1" >&2; exit 99 ;;
esac
STUB
chmod +x "$tmp/bin/sudo"

reset() {
  rm -rf "$tmp/runner" "$tmp/archives" "$tmp/log"
  mkdir -p "$tmp/runner" "$tmp/archives"
  : >"$tmp/log"
}

run() { # $1 label, $2 expected rc, rest: script args
  local label="$1" want="$2" rc=0
  shift 2
  PATH="$tmp/bin:$PATH" STUB_LOG="$tmp/log" RUNNER_TEMP="$tmp/runner" \
    APT_ARCHIVE_DIR="$tmp/archives" bash "$script" "$@" >/dev/null 2>&1 || rc=$?
  if [ "$rc" -eq "$want" ]; then
    echo "ok   $label (rc=$rc)"
    pass=$((pass + 1))
  else
    echo "FAIL $label: expected rc=$want got rc=$rc" >&2
    fail=$((fail + 1))
  fi
}

check() { # $1 label, $2 grep pattern that must be in the log
  if grep -q -- "$2" "$tmp/log"; then
    echo "ok   $1"
    pass=$((pass + 1))
  else
    echo "FAIL $1: no '$2' in log" >&2
    fail=$((fail + 1))
  fi
}

refute() { # $1 label, $2 grep pattern that must not be in the log
  if grep -q -- "$2" "$tmp/log"; then
    echo "FAIL $1: unexpected '$2' in log" >&2
    fail=$((fail + 1))
  else
    echo "ok   $1"
    pass=$((pass + 1))
  fi
}

printf '# comment\npkg-a\npkg-b\n' >"$tmp/list.txt"

# Positive: list installed with extra args, cache restored and saved back.
reset
mkdir -p "$tmp/runner/apt-archives"
: >"$tmp/runner/apt-archives/cached.deb"
run "install with warm cache" 0 install "$tmp/list.txt" --no-install-recommends
check "retry config written" 'Acquire::Retries "3"'
check "apt-get update ran" '^sudo apt-get update$'
check "packages and args reach apt" '^sudo apt-get install -y --no-install-recommends pkg-a pkg-b$'
refute "comment line not installed" 'install.*# comment'
check "cache restored before install" "^sudo cp -f $tmp/runner/apt-archives/cached.deb $tmp/archives/$"
if [ -f "$tmp/runner/apt-archives/fresh.deb" ]; then
  echo "ok   new archives saved to cache"
  pass=$((pass + 1))
else
  echo "FAIL new archives not saved to cache" >&2
  fail=$((fail + 1))
fi

# Boundary: cold cache, nothing to restore, install still runs.
reset
run "install with cold cache" 0 install "$tmp/list.txt"
refute "no restore from empty cache" "^sudo cp -f $tmp/runner/apt-archives/"
check "cold install reaches apt" '^sudo apt-get install -y pkg-a pkg-b$'

# configure writes the config only.
reset
run "configure only" 0 configure
check "configure writes retry config" '80ci-retries'
refute "configure does not touch apt" 'apt-get'

# Negative cases.
reset
run "missing list" 66 install "$tmp/nope.txt"
refute "missing list stops before apt" 'apt-get'
run "no arguments" 64
run "unknown subcommand" 64 upgrade
run "install without list" 64 install
run "configure with extra argument" 64 configure extra
reset
STUB_APT_FAIL=1 run "apt install failure fails the step (xargs 123)" 123 install "$tmp/list.txt"
reset
rc=0
PATH="$tmp/bin:$PATH" STUB_LOG="$tmp/log" APT_ARCHIVE_DIR="$tmp/archives" \
  env -u RUNNER_TEMP bash "$script" install "$tmp/list.txt" >/dev/null 2>&1 || rc=$?
if [ "$rc" -ne 0 ]; then
  echo "ok   unset RUNNER_TEMP fails (rc=$rc)"
  pass=$((pass + 1))
else
  echo "FAIL unset RUNNER_TEMP passed" >&2
  fail=$((fail + 1))
fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
