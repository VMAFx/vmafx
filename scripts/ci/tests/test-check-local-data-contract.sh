#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Hermetic positive and negative cases for ADR-1277.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
script="$here/../check-local-data-contract.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

state_root=.workingdir
corpus_root=.corpus
retired_root="${state_root}2"
case_number=0
case_dir=

new_case() {
  case_number=$((case_number + 1))
  case_dir="$tmp/case-$case_number"
  mkdir -p "$case_dir"
  git -C "$case_dir" init -q
  git -C "$case_dir" config user.email test@example.invalid
  git -C "$case_dir" config user.name test
  printf '%s/\n%s/\n' "$state_root" "$corpus_root" >"$case_dir/.gitignore"
  printf '%s/\n%s/\n%s/\n' \
    "$state_root" "$retired_root" "$corpus_root" >"$case_dir/.dockerignore"
  git -C "$case_dir" add .gitignore .dockerignore
}

track() {
  local path=$1 content=$2
  mkdir -p "$case_dir/$(dirname "$path")"
  printf '%s\n' "$content" >"$case_dir/$path"
  git -C "$case_dir" add "$path"
}

expect() {
  local label=$1 expected=$2 rc=0 output
  output=$(cd "$case_dir" && bash "$script" 2>&1) || rc=$?
  if [ "$rc" -ne "$expected" ]; then
    printf 'FAIL %s: expected rc=%s got rc=%s\n%s\n' \
      "$label" "$expected" "$rc" "$output" >&2
    exit 1
  fi
  printf 'ok   %s (rc=%s)\n' "$label" "$rc"
}

new_case
track docs/note.md "state .workingdir/OPEN.md; dataset .corpus/netflix/"
expect 'plain operator paths are allowed' 0

new_case
printf '%s/\n' "$corpus_root" >"$case_dir/.gitignore"
git -C "$case_dir" add .gitignore
expect 'state root must be ignored' 1

new_case
printf '%s/\n' "$state_root" >"$case_dir/.gitignore"
git -C "$case_dir" add .gitignore
expect 'corpus root must be ignored' 1

new_case
printf '%s*/\n%s/\n' "$state_root" "$corpus_root" >"$case_dir/.gitignore"
git -C "$case_dir" add .gitignore
expect 'wildcard must not hide the retired root' 1

new_case
printf '%s/\n%s/\n' "$state_root" "$corpus_root" >"$case_dir/.dockerignore"
expect 'retired root stays outside Docker contexts' 1

new_case
track scripts/config.txt "cache=$retired_root/cache"
expect 'active retired-path reference fails' 1

new_case
track docs/adr/0003-history.md "historical path: \`$retired_root/\`"
expect 'plain historical ADR text remains truthful' 0

new_case
track docs/current-audit.md "current authority: \`$retired_root/OPEN.md\`"
expect 'top-level public docs reject the retired path' 1

new_case
track docs/adr/_index_fragments/_header.md "current path: \`$retired_root/\`"
expect 'current ADR index header rejects retired path' 1

new_case
mkdir -p "$case_dir/$retired_root"
expect 'untracked retired directory fails visibly' 1

new_case
mkdir -p "$case_dir/$retired_root"
printf 'state\n' >"$case_dir/$retired_root/OPEN.md"
git -C "$case_dir" add -f "$retired_root/OPEN.md"
expect 'tracked retired directory fails' 1

new_case
mkdir -p "$case_dir/$state_root"
printf 'state\n' >"$case_dir/$state_root/OPEN.md"
git -C "$case_dir" add -f "$state_root/OPEN.md"
expect 'tracked state root fails' 1

new_case
mkdir -p "$case_dir/$corpus_root"
printf 'data\n' >"$case_dir/$corpus_root/clip.jsonl"
git -C "$case_dir" add -f "$corpus_root/clip.jsonl"
expect 'tracked corpus root fails' 1

new_case
track docs/note.md "[private state](../$state_root/OPEN.md)"
expect 'Markdown link into state fails' 1

new_case
track docs/note.md "[private corpus](../$corpus_root/netflix/)"
expect 'Markdown link into corpus fails' 1

new_case
track scripts/config.txt '.workingdir/netflix/canonical.jsonl'
expect 'corpus placed below state root fails' 1

new_case
track scripts/config.txt '.corpus/OPEN.md'
expect 'state placed below corpus root fails' 1

printf 'all local-data contract cases passed\n'
