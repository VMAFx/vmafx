#!/usr/bin/env bash
# Render docs/rebase-notes.d/*.md into the fragment block of docs/rebase-notes.md.
#
# Inputs:
#   docs/rebase-notes.d/<slug>.md   one rebase note per pull request, starting
#       with its `## <title> (<date>)` heading. A file whose name starts with
#       `_` is not rendered.
#
# Output: the block between the two markers in docs/rebase-notes.md, newest
# first. Everything outside the markers (the front matter, the title and the
# entries written before the notes moved to fragments) is left as it is.
# "Newest" is the order the fragments landed on the branch, which
# scripts/docs/fragment-order.py reads from history (ADR-2197).
#
# Flags:
#   --check   exit 1 when the block differs from the fragments.
#   --write   rewrite the block.
#   --lint    check the fragment files only (a pull request runs this).
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
FRAG_ROOT="$REPO_ROOT/docs/rebase-notes.d"
TARGET="$REPO_ROOT/docs/rebase-notes.md"
BEGIN='<!-- rebase-notes:fragments:begin (rendered from docs/rebase-notes.d/; do not edit) -->'
END='<!-- rebase-notes:fragments:end -->'

fragments() {
  python3 "$SCRIPT_DIR/fragment-order.py" "$FRAG_ROOT" --glob '*.md' | tac
}

lint() {
  local name rc=0 first
  while IFS= read -r name; do
    first="$(awk 'NF { print; exit }' "$FRAG_ROOT/$name")"
    if [[ "$first" != '## '* ]]; then
      printf 'rebase-notes fragment %s must start with a "## <title> (<date>)" heading\n' "$name" >&2
      rc=1
    fi
  done < <(find "$FRAG_ROOT" -maxdepth 1 -type f -name '*.md' ! -name '_*' -printf '%f\n' | LC_ALL=C sort)
  return "$rc"
}

render() {
  local name
  while IFS= read -r name; do
    [[ -z "$name" ]] && continue
    cat "$FRAG_ROOT/$name"
    [[ "$(tail -c1 "$FRAG_ROOT/$name")" == $'\n' ]] || printf '\n'
    printf '\n'
  done < <(fragments)
}

mode="render"
case "${1:-}" in
  --check) mode="check" ;;
  --write) mode="write" ;;
  --lint)
    lint
    exit $?
    ;;
  --help | -h)
    sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
  "") ;;
  *)
    printf 'unknown flag: %s\n' "$1" >&2
    exit 64
    ;;
esac

lint
if [[ "$mode" == render ]]; then
  render
  exit 0
fi

has_markers() {
  [[ "$(grep -cxF "$BEGIN" "$TARGET")" -eq 1 && "$(grep -cxF "$END" "$TARGET")" -eq 1 ]]
}

if ! has_markers; then
  if [[ "$(grep -cxF "$BEGIN" "$TARGET")" -ne 0 || "$(grep -cxF "$END" "$TARGET")" -ne 0 ]]; then
    printf 'docs/rebase-notes.md has an unpaired or repeated fragment marker\n' >&2
    exit 1
  fi
  if [[ "$mode" == check ]]; then
    printf 'docs/rebase-notes.md has no fragment block yet.\nRun: make docs-render\n' >&2
    exit 1
  fi
fi

tmp_body="$(mktemp)"
tmp_out="$(mktemp)"
trap 'rm -f "$tmp_body" "$tmp_out"' EXIT
render >"$tmp_body"

if ! has_markers; then
  # First render (ADR-2197): the block is added under the title, above the
  # entries written before the notes moved to fragments.
  awk -v begin="$BEGIN" -v end="$END" '
    !done && /^# Rebase notes[[:space:]]*$/ { print; print ""; print begin; print end; done = 1; next }
    { print }
  ' "$TARGET" >"$tmp_out"
  if ! grep -qxF "$BEGIN" "$tmp_out"; then
    printf 'docs/rebase-notes.md has no "# Rebase notes" heading to put the block under\n' >&2
    exit 1
  fi
  cat "$tmp_out" >"$TARGET"
fi

awk -v begin="$BEGIN" -v end="$END" -v body="$tmp_body" '
  $0 == begin { print; while ((getline line < body) > 0) print line; close(body); skip = 1; next }
  $0 == end { skip = 0; print; next }
  !skip { print }
' "$TARGET" >"$tmp_out"

if [[ "$mode" == check ]]; then
  if diff -u "$TARGET" "$tmp_out" >/dev/null; then
    exit 0
  fi
  { diff -u "$TARGET" "$tmp_out" || [ "$?" -eq 1 ]; } | head -40
  printf '\ndocs/rebase-notes.md is out of sync with docs/rebase-notes.d/.\n' >&2
  printf 'Run: make docs-render\n' >&2
  exit 1
fi

cat "$tmp_out" >"$TARGET"
printf 'docs/rebase-notes.md fragment block rewritten from docs/rebase-notes.d/.\n' >&2
