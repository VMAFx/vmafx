#!/usr/bin/env bash
# Tests for scripts/ci/check-no-tracked-venv.sh: a throwaway repo with tracked
# paths; the gate must flag real virtualenv paths and pass look-alike names.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
script="$here/../check-no-tracked-venv.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
git -C "$tmp" init -q
mk() {
  mkdir -p "$tmp/$(dirname "$1")"
  printf 'x\n' >"$tmp/$1"
  git -C "$tmp" add "$1"
}
expect() { # $1 label, $2 expected rc
  local rc=0
  (cd "$tmp" && bash "$script" >/dev/null 2>&1) || rc=$?
  if [ "$rc" -ne "$2" ]; then
    echo "FAIL $1: expected rc=$2 got rc=$rc" >&2
    exit 1
  fi
  echo "ok   $1 (rc=$rc)"
}
mk changelog.d/fixed/venv-recipe-docs.md
expect "look-alike basename venv-recipe-docs.md passes" 0
mk docs/venvs.md
expect "look-alike venvs.md passes" 0
mk tools/inventory/pyvenv-notes.md
expect "look-alike pyvenv-notes.md passes" 0
mk pyvenv.cfg
expect "pyvenv.cfg marker flagged" 1
git -C "$tmp" rm -q --cached pyvenv.cfg
mk .venv-linux/bin/python
expect "file inside .venv-linux/ flagged" 1
git -C "$tmp" rm -q --cached .venv-linux/bin/python
mk venv/lib/site.py
expect "file inside venv/ flagged" 1
git -C "$tmp" rm -q --cached venv/lib/site.py
ln -s /nonexistent "$tmp/.venv"
git -C "$tmp" add .venv
expect ".venv symlink flagged" 1
echo "all check-no-tracked-venv cases passed"
