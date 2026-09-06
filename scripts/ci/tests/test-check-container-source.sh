#!/usr/bin/env bash
# Tests for scripts/dev/check-container-source.sh (ADR-1195).
#
# Hermetic: builds throwaway git repositories in a temp dir. The --image mode
# needs Docker, so it is exercised only when Docker is present; the --pre-build
# mode, which is the one that catches a stale build context, always runs.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
script="$here/../../dev/check-container-source.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

pass=0
expect() { # $1 label, $2 expected rc, $3 workdir, rest: args
  local label="$1" want="$2" dir="$3"
  shift 3
  local rc=0
  (cd "$dir" && bash "$script" "$@" >/dev/null 2>&1) || rc=$?
  if [[ "$rc" -ne "$want" ]]; then
    echo "FAIL $label: expected rc=$want got rc=$rc" >&2
    exit 1
  fi
  echo "ok   $label (rc=$rc)"
  pass=$((pass + 1))
}

# A repo with an "origin/master" that HEAD can be moved around relative to.
# A real remote is avoided: the script only ever resolves a ref, so a local
# ref named refs/remotes/origin/master is indistinguishable to it, and this
# keeps the test offline.
make_repo() { # $1 dir
  local d="$1"
  mkdir -p "$d/core"
  git -C "$d" init --quiet -b master
  git -C "$d" config user.email t@example.com
  git -C "$d" config user.name t
  echo one >"$d/core/a.c"
  git -C "$d" add -A
  git -C "$d" commit --quiet -m "first"
  git -C "$d" update-ref refs/remotes/origin/master HEAD
}

make_repo "$tmp/current"
expect "a checkout level with the ref may build" 0 "$tmp/current" --pre-build --no-fetch

# Advance origin/master past HEAD, touching a baked-in path: the exact shape of
# the 2026-09-06 miss, where the context was behind and the image looked fresh.
make_repo "$tmp/behind"
echo two >"$tmp/behind/core/a.c"
git -C "$tmp/behind" commit --quiet -am "second"
git -C "$tmp/behind" update-ref refs/remotes/origin/master HEAD
git -C "$tmp/behind" reset --quiet --hard HEAD~1
expect "a checkout behind the ref is refused" 1 "$tmp/behind" --pre-build --no-fetch

# Ahead is not stale: a feature branch legitimately leads master.
make_repo "$tmp/ahead"
echo two >"$tmp/ahead/core/a.c"
git -C "$tmp/ahead" commit --quiet -am "second"
expect "a checkout ahead of the ref may build" 0 "$tmp/ahead" --pre-build --no-fetch

# A dirty tree warns but does not block: local experiments are legitimate.
make_repo "$tmp/dirty"
echo scratch >"$tmp/dirty/core/a.c"
expect "uncommitted changes warn but do not block" 0 "$tmp/dirty" --pre-build --no-fetch

# Unresolvable ref is "cannot tell", not "fine".
expect "an unresolvable ref is rc=2" 2 "$tmp/current" --pre-build --no-fetch --ref refs/heads/nope

# Outside a repository the script must not claim anything.
mkdir -p "$tmp/norepo"
expect "outside a git repo is rc=2" 2 "$tmp/norepo" --pre-build --no-fetch

expect "an unknown argument is rejected" 2 "$tmp/current" --frobnicate

if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
  # An image with no marker must be rc=2 (unverifiable), never rc=0.
  expect "an image without the marker is rc=2" 2 "$tmp/current" \
    --image busybox:latest --no-fetch
  echo "ok   docker-backed cases ran"
else
  echo "skip docker-backed cases (no usable docker daemon)"
fi

echo "all check-container-source cases passed ($pass assertions)"
