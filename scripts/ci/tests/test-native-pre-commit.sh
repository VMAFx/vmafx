#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Tests for scripts/githooks/pre-commit.sh in a throwaway repo: a staged shell
# file that shfmt would rewrite is rewritten and re-staged, an already formatted
# one is left alone, and a missing formatter is skipped with a notice. The
# formatter is a fake `shfmt` on PATH, so the test needs no toolchain.
set -euo pipefail

# Drop the git hook environment and the fixture identity (see the helper).
# shellcheck source=/dev/null
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/../../lib/clean-git-env.sh"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
hook="$here/../../githooks/pre-commit.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

fail() {
  echo "FAIL $1" >&2
  exit 1
}

mkdir -p "$tmp/bin" "$tmp/repo"
# Fake shfmt: `-w ... -- FILE...` rewrites every file whose first line is
# "needs-format" to "formatted".
cat >"$tmp/bin/shfmt" <<'SH'
#!/usr/bin/env bash
while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do shift; done
shift
for f in "$@"; do
  if [ "$(head -n 1 "$f")" = "needs-format" ]; then
    printf 'formatted\n' >"$f"
  fi
done
SH
chmod +x "$tmp/bin/shfmt"

cd "$tmp/repo"
git init -q
printf 'needs-format\n' >a.sh
printf 'clean\n' >b.sh
git add a.sh b.sh

# Only the fake shfmt may be found; the real git and coreutils stay reachable.
run_hook() {
  PATH="$tmp/bin:$PATH" bash "$hook" 2>&1
}

out="$(run_hook)" || fail "hook failed: $out"
[ "$(git show :a.sh)" = "formatted" ] || fail "a.sh was not re-staged formatted: $(git show :a.sh)"
[ "$(git show :b.sh)" = "clean" ] || fail "b.sh changed although already formatted"
case "$out" in *"re-staged 1 file"*) ;; *) fail "summary lacks 're-staged 1 file': $out" ;; esac
echo "ok   a changed file is re-staged, an unchanged one is left alone"

rm "$tmp/bin/shfmt"
printf 'needs-format\n' >c.sh
git add c.sh
# Keep the real PATH minus any shfmt: shadow it with a directory that has none.
shadow="$tmp/noshfmt"
mkdir -p "$shadow"
for tool in git bash head cat printf mapfile dirname sed grep; do
  if path="$(command -v "$tool" 2>/dev/null)" && [ -x "$path" ]; then
    ln -sf "$path" "$shadow/$tool"
  fi
done
out="$(PATH="$shadow" bash "$hook" 2>&1)" || fail "hook failed without shfmt: $out"
case "$out" in *"shfmt: not installed"*) ;; *) fail "missing formatter not reported: $out" ;; esac
[ "$(git show :c.sh)" = "needs-format" ] || fail "c.sh changed without a formatter"
echo "ok   a missing formatter is skipped with a notice"
