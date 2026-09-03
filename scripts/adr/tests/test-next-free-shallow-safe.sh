#!/usr/bin/env bash
# Regression test for the ADR allocator's shallow-safety guard.
#
# `scripts/adr/next-free.sh` fetches from origin on every claim. Before this
# guard it passed `--depth` unconditionally, which CONVERTS a full clone into a
# shallow one: `.git/shallow` appears, `git merge-base` stops resolving, and
# every rebase in the repository and all of its worktrees reports the whole tree
# as conflicting. CI checkouts are already shallow and keep the smaller fetch.
set -euo pipefail
export LC_ALL=C GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t
SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/adr/next-free.sh"
fail=0
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# an upstream with enough history that --depth would visibly truncate it
git init -q --initial-branch=master "$tmp/upstream"
(
  cd "$tmp/upstream"
  mkdir -p docs/adr
  for i in 1 2 3 4 5; do
    printf '# ADR-000%s\n' "$i" >"docs/adr/000$i-t.md"
    git add -A && git commit -q -m "adr $i"
  done
)

# a FULL clone, exactly like a developer's checkout
git clone -q "$tmp/upstream" "$tmp/full"
(
  cd "$tmp/full"
  [ "$(git rev-parse --is-shallow-repository)" = false ] || {
    echo "FAIL: fixture clone is already shallow"
    exit 1
  }
)

# the allocator's own fetch pattern, through the guard
(
  cd "$tmp/full"
  # shellcheck disable=SC1090  # the script under test is resolved at runtime
  depth_fn="$(sed -n '/^adr_depth_args()/,/^}/p' "$SCRIPT")"
  eval "$depth_fn"
  mapfile -t d < <(adr_depth_args --depth=50)
  git fetch origin master "${d[@]}" --quiet 2>/dev/null || true
)

if [ "$(git -C "$tmp/full" rev-parse --is-shallow-repository)" = true ]; then
  echo "FAIL: a full clone became shallow after the allocator's fetch"
  fail=1
else
  echo "PASS: full clone stays complete (no --depth applied)"
fi

# a genuinely shallow checkout, like CI: the depth flag must still be used
git clone -q --depth=1 "file://$tmp/upstream" "$tmp/shallow" 2>/dev/null
if [ "$(git -C "$tmp/shallow" rev-parse --is-shallow-repository)" = true ]; then
  depth_fn="$(sed -n '/^adr_depth_args()/,/^}/p' "$SCRIPT")"
  (
    cd "$tmp/shallow"
    eval "$depth_fn"
    mapfile -t d < <(adr_depth_args --depth=50)
    if [ "${#d[@]}" -eq 1 ] && [ "${d[0]}" = "--depth=50" ]; then
      echo "PASS: shallow checkout still gets --depth=50"
    else
      echo "FAIL: shallow checkout lost its depth flag (got: ${d[*]:-none})"
      exit 1
    fi
  ) || fail=1
else
  echo "SKIP: could not build a shallow fixture on this filesystem"
fi

[ "$fail" -eq 0 ] && echo "=== test-next-free-shallow-safe: all checks passed ==="
exit "$fail"
