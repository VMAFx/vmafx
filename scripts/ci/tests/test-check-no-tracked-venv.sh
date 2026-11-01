#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Tests for scripts/ci/check-no-tracked-venv.sh: a throwaway repo with tracked
# paths; the gate must flag real virtualenv paths and pass look-alike names.
set -euo pipefail

# Drop the git hook environment and the fixture identity (see the helper).
# shellcheck source=/dev/null
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/../../lib/clean-git-env.sh"
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
git -C "$tmp" rm -q --cached .venv
rm "$tmp/.venv"

# Untracked strays in the worktree root (a bundled uv drops both).
mkdir -p "$tmp/.venv/bin"
expect "stray root .venv without bin/pip flagged" 1
touch "$tmp/.venv/bin/pip"
expect "make-built root .venv (bin/pip) passes" 0
rm -r "$tmp/.venv"
ln -s /nonexistent "$tmp/.venv"
expect "untracked dangling root .venv symlink flagged" 1
rm "$tmp/.venv"
mkdir -p "$tmp/.venv/bin"
touch "$tmp/.venv/bin/pip"
printf '[[package]]\nname = "vmafx-tooling"\nsource = { virtual = "." }\n' >"$tmp/uv.lock"
expect "package-less root uv.lock flagged" 1
printf '[[package]]\nname = "pyyaml"\n' >>"$tmp/uv.lock"
expect "uv.lock that locks packages passes" 0
rm "$tmp/uv.lock"

# The Makefile puts the root .venv first on PATH unless it is a stray.
root="$here/../../.."
probe() { # $1 make variable to print: the first PATH entry or the python3 recipes resolve
  local out
  # shellcheck disable=SC2016 # recipe text: the recipe shell expands it, not this one
  out=$(make -s -C "$tmp" -f "$root/Makefile" --eval 'probe-py: ; @command -v python3' \
    --eval 'probe-path: ; @printf "%s\n" "$${PATH%%:*}"' "$1") || {
    echo "FAIL make probe $1 exited nonzero" >&2
    exit 1
  }
  printf '%s\n' "$out"
}
printf '#!/bin/sh\nexit 0\n' >"$tmp/.venv/bin/python3"
chmod +x "$tmp/.venv/bin/python3"
got=$(probe probe-py) || exit 1
case "$got" in
  "$tmp/.venv/bin/python3") echo "ok   make-built venv goes first on PATH" ;;
  *)
    echo "FAIL make-built venv not first on PATH" >&2
    exit 1
    ;;
esac
rm "$tmp/.venv/bin/pip"
got=$(probe probe-py) || exit 1
case "$got" in
  "$tmp/.venv/bin/python3")
    echo "FAIL stray venv shadows python3 under make" >&2
    exit 1
    ;;
  *) echo "ok   stray venv stays off PATH under make" ;;
esac
rm -r "$tmp/.venv"
got=$(probe probe-path) || exit 1
case "$got" in
  "$tmp/.venv/bin") echo "ok   venv path goes first before make builds it" ;;
  *)
    echo "FAIL venv path not first before make builds it (got $got)" >&2
    exit 1
    ;;
esac
echo "all check-no-tracked-venv cases passed"
