#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Synchronize shared private state while preserving the committing worktree's
# Git identity. Praetor rejects symlinked confinement roots, so linked
# worktrees use a regular-file mirror under an exclusive common-Git lock.
set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
git_common_dir=$(git rev-parse --path-format=absolute --git-common-dir)
canonical_root=$(dirname "$git_common_dir")
canonical_state="$canonical_root/.workingdir"
local_state="$repo_root/.workingdir"
lock_dir="$git_common_dir/vmafx-state-sync.lock"
mirror_tmp=""
canonical_tmp=""
lock_acquired=0

cleanup() {
  if [ -n "$canonical_tmp" ] && [ -e "$canonical_tmp" ]; then
    rm -f -- "$canonical_tmp"
  fi
  if [ -n "$mirror_tmp" ] && [ -d "$mirror_tmp" ]; then
    rm -rf -- "$mirror_tmp"
  fi
  if [ "$lock_acquired" -eq 1 ]; then
    rmdir "$lock_dir"
  fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

if ! mkdir "$lock_dir" 2>/dev/null; then
  echo "state-sync: another state synchronization owns $lock_dir" >&2
  exit 1
fi
lock_acquired=1

ledger_names=(OPEN.md BACKLOG.md BUGS.md QUESTIONS.md STATE.md bugs.meta.json)
for name in "${ledger_names[@]}"; do
  source_path="$canonical_state/$name"
  if [ ! -f "$source_path" ] || [ -L "$source_path" ]; then
    echo "state-sync: canonical ledger must be a regular file: $source_path" >&2
    exit 1
  fi
done

run_state_sync() {
  local target=$1
  if command -v praetorctl >/dev/null 2>&1; then
    (cd "$target" && praetorctl state sync "$target")
  elif [ -d "$repo_root/cmd/standardsctl" ]; then
    (cd "$repo_root" && go run ./cmd/standardsctl state sync "$target")
  else
    echo "state-sync: praetorctl is unavailable and cmd/standardsctl is absent" >&2
    return 1
  fi
}

if [ "$repo_root" = "$canonical_root" ]; then
  run_state_sync "$repo_root"
  exit 0
fi

if [ -L "$local_state" ]; then
  echo "state-sync: linked-worktree .workingdir must be a directory, not a symlink: $local_state" >&2
  exit 1
fi
mkdir -p "$local_state"
mirror_tmp=$(mktemp -d "$local_state/.state-sync.XXXXXX")
for name in "${ledger_names[@]}"; do
  cp -p -- "$canonical_state/$name" "$mirror_tmp/$name"
done
for name in "${ledger_names[@]}"; do
  mv -f -- "$mirror_tmp/$name" "$local_state/$name"
done
rmdir "$mirror_tmp"
mirror_tmp=""

run_state_sync "$repo_root"

canonical_tmp=$(mktemp "$canonical_state/.STATE.md.sync.XXXXXX")
cp -p -- "$local_state/STATE.md" "$canonical_tmp"
mv -f -- "$canonical_tmp" "$canonical_state/STATE.md"
canonical_tmp=""
