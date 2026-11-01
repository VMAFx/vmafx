#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Refuse to let a virtualenv — directory OR symlink — into the tree.
#
# `.gitignore`'s `.venv*/` matches only directories. #1231 committed a symlink
# named `.venv` that pointed at an absolute path on the author's machine; on
# every other checkout it resolved to itself, and because git treats ignored
# paths as disposable, `git pull` silently replaced developers' real venvs with
# a self-referential loop that then broke every PATH-searched exec (glibc's
# execvp aborts on ELOOP rather than skipping the directory). The .gitignore
# now also carries `.venv*`, but an ignore rule only stops `git add` from
# picking a path up — it does nothing about a path that is already tracked,
# and nothing at all against `git add -f`. This gate is the backstop.
#
# Without REF the gate also refuses two untracked strays in the worktree root
# that a bundled `uv` drops there: a `.venv` that is not the Makefile's (no
# `bin/pip`; `make` puts the root `.venv` first on PATH, so its bare `python3`
# lacked `jsonschema` and `make docs-fragments-check` failed), and a `uv.lock`
# that locks no package beyond the root project (the tree's locks live in
# `requirements/locks/`).
#
# Usage: check-no-tracked-venv.sh [REF]   (default: the index and worktree root)
set -euo pipefail
export LC_ALL=C

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

ref="${1:-}"
if [ -n "$ref" ]; then
  listing=$(git ls-tree -r --name-only "$ref")
else
  listing=$(git ls-files)
fi

# Two shapes are virtualenv paths: (1) a tracked entry whose basename IS a venv
# directory/symlink or marker — `.venv`, `.venv-foo`, `venv`, `.virtualenv`,
# `pyvenv.cfg` (a symlink shows up as a plain path in ls-files); (2) a tracked
# file INSIDE such a directory (`venv/bin/python`, `.venv-x/lib/...`). A basename
# that merely starts with "venv" (e.g. changelog.d/fixed/venv-recipe-docs.md) is
# not a virtualenv — the previous pattern's optional dot matched it (#1282).
bad=$(printf '%s\n' "$listing" | grep -E '(^|/)(\.venv[^/]*|venv|\.virtualenv|pyvenv\.cfg)$|(^|/)(\.venv[^/]*|venv|\.virtualenv)/' || [ "$?" -eq 1 ])
if [ -n "$bad" ]; then
  printf 'error: virtualenv path(s) tracked in git — these must never be committed:\n' >&2
  printf '%s\n' "$bad" | sed 's/^/  /' >&2
  printf '\nRemove with: git rm --cached <path>   (.gitignore already excludes them)\n' >&2
  exit 1
fi
if [ -z "$ref" ]; then
  stray=""
  # -L too: a dangling .venv symlink fails -d but still blocks `python3 -m venv`.
  if { [ -d .venv ] || [ -L .venv ]; } && [ ! -e .venv/bin/pip ]; then
    stray=".venv (no bin/pip: not the venv \`make\` builds)"
  fi
  if [ -f uv.lock ] && ! git ls-files --error-unmatch uv.lock >/dev/null 2>&1; then
    packages=$(grep -c '^\[\[package\]\]' uv.lock || [ "$?" -eq 1 ])
    if [ "${packages:-0}" -le 1 ]; then
      stray="${stray:+$stray
}uv.lock (locks no package beyond the root project)"
    fi
  fi
  if [ -n "$stray" ]; then
    printf 'error: stray virtualenv state in the worktree root:\n' >&2
    printf '%s\n' "$stray" | sed 's/^/  /' >&2
    printf '\nA tool outside the build created it; it shadows python3 under make.\n' >&2
    printf 'Remove the path(s) above; make then builds its own venv.\n' >&2
    exit 1
  fi
fi
printf 'check-no-tracked-venv: OK (no virtualenv paths tracked, no stray root venv)\n'
