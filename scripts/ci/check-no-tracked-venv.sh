#!/usr/bin/env bash
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
# Usage: check-no-tracked-venv.sh [REF]   (default: the index)
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

bad=$(printf '%s\n' "$listing" | grep -E '(^|/)(\.?venv[^/]*|\.virtualenv|pyvenv\.cfg)$' || true)
if [ -n "$bad" ]; then
  printf 'error: virtualenv path(s) tracked in git — these must never be committed:\n' >&2
  printf '%s\n' "$bad" | sed 's/^/  /' >&2
  printf '\nRemove with: git rm --cached <path>   (.gitignore already excludes them)\n' >&2
  exit 1
fi
printf 'check-no-tracked-venv: OK (no virtualenv paths tracked)\n'
