#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Synchronize shared private state while preserving the committing worktree's
# Git identity. Praetor rejects symlinked confinement roots, so linked
# worktrees use a regular-file mirror under an exclusive common-Git lock. The
# lock names its owner (pid and boot); a lock whose owner ran in another boot
# or no longer runs is stale, and the next sync takes it over and says so.
set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
git_common_dir=$(git rev-parse --path-format=absolute --git-common-dir)
canonical_root=$(dirname "$git_common_dir")
canonical_state="$canonical_root/.workingdir"
local_state="$repo_root/.workingdir"
lock_dir="$git_common_dir/vmafx-state-sync.lock"
lock_owner="$lock_dir/owner"
# An ownerless lock younger than this is a holder between its mkdir and its
# owner file; an older one is a lock this script's earlier versions left.
lock_grace_minutes=2
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
    rm -f -- "$lock_owner" "$lock_owner.$$"
    rmdir "$lock_dir"
  fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# The boot this host runs: Linux's boot_id, macOS's boot session; "unknown"
# where neither exists, and then only the owner's process decides.
boot_id() {
  if [ -r /proc/sys/kernel/random/boot_id ]; then
    cat /proc/sys/kernel/random/boot_id
  elif sysctl -n kern.bootsessionuuid 2>/dev/null; then
    :
  else
    echo unknown
  fi
}

# The owner record of a lock directory; nothing when it has none (yet, or no
# longer: a holder removes it on release).
owner_of() {
  local record=""
  if [ -f "$1/owner" ] && record=$(cat -- "$1/owner" 2>/dev/null); then
    printf '%s\n' "$record"
  fi
}

pid_alive() {
  [ -n "$1" ] || return 1
  kill -0 "$1" 2>/dev/null && return 0
  command -v ps >/dev/null 2>&1 && ps -p "$1" >/dev/null 2>&1
}

# Prints why the lock a holder left is stale, and fails when it is not: its
# owner ran in another boot or is no longer running, or it has no owner file
# and is older than the grace a holder needs to write one.
lock_stale_reason() {
  local owner=$1 pid boot
  pid=$(printf '%s\n' "$owner" | sed -n 's/^pid=//p')
  boot=$(printf '%s\n' "$owner" | sed -n 's/^boot=//p')
  if [ -z "$pid" ]; then
    if [ -n "$(find "$lock_dir" -maxdepth 0 -mmin +"$lock_grace_minutes" 2>/dev/null)" ]; then
      echo "it has no owner and is older than $lock_grace_minutes minutes"
      return 0
    fi
    return 1
  fi
  if [ -n "$boot" ] && [ "$boot" != unknown ] && [ "$current_boot" != unknown ] &&
    [ "$boot" != "$current_boot" ]; then
    echo "its owner, pid $pid, ran in an earlier boot ($boot)"
    return 0
  fi
  if ! pid_alive "$pid"; then
    echo "its owner, pid $pid, is not running"
    return 0
  fi
  return 1
}

# Moves a stale lock aside under a name of this process and removes it, but
# only if it is still the lock that was judged: a holder that took the lock
# over in between gets it back.
take_over_stale_lock() {
  local judged=$1 reason=$2 aside="$lock_dir.stale.$$"
  mv -- "$lock_dir" "$aside" 2>/dev/null || return 1
  if [ "$(owner_of "$aside")" != "$judged" ]; then
    if [ ! -e "$lock_dir" ] && ! mv -- "$aside" "$lock_dir" 2>/dev/null; then
      echo "state-sync: could not return the lock moved to $aside" >&2
    fi
    return 1
  fi
  rm -rf -- "$aside"
  echo "state-sync: took over the stale lock $lock_dir: $reason" >&2
}

acquire_lock() {
  local owner reason
  mkdir "$lock_dir" 2>/dev/null && return 0
  owner=$(owner_of "$lock_dir")
  reason=$(lock_stale_reason "$owner") || return 1
  take_over_stale_lock "$owner" "$reason" || return 1
  mkdir "$lock_dir" 2>/dev/null
}

current_boot=$(boot_id)
if ! acquire_lock; then
  owner=$(owner_of "$lock_dir" | tr '\n' ' ')
  echo "state-sync: another state synchronization owns $lock_dir (${owner:-no owner file yet})" >&2
  exit 1
fi
lock_acquired=1
printf 'pid=%s\nboot=%s\n' "$$" "$current_boot" >"$lock_owner.$$"
mv -f -- "$lock_owner.$$" "$lock_owner"

# questions.meta.json carries the metadata praetor's state sync reads for every
# QUESTIONS.md entry; without it a linked worktree's sync fails
# ("Q-001 metadata missing from questions.meta.json").
ledger_names=(OPEN.md BACKLOG.md BUGS.md QUESTIONS.md STATE.md bugs.meta.json questions.meta.json)
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
