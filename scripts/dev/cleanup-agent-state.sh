#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# cleanup-agent-state.sh — sweep stale agent worktrees and stashes
#
# Removes git worktrees whose lock-holder PID is no longer alive.
# Drops stashes that are redundant (branch still exists locally) or are
# clearly ephemeral agent navigation checkpoints on "master" or "no branch".
#
# Usage:
#   scripts/dev/cleanup-agent-state.sh [--dry-run]
#
# Dry-run mode prints what would be removed without changing anything.
#
# The script never touches:
#   - Worktrees owned by alive PIDs
#   - Stashes on BRANCH_GONE branches (the only copy of the work)
#   - The main worktree
#
# Run from any worktree of the repo. git -C REPO_ROOT is used throughout.

set -euo pipefail

DRY_RUN=false
if [[ "${1:-}" == "--dry-run" ]]; then
  DRY_RUN=true
fi

REPO_ROOT=$(git rev-parse --show-toplevel)

log() { echo "[cleanup] $*"; }
info() { echo "[info]    $*"; }

###############################################################################
# 1. Worktree cleanup: remove agent-* worktrees whose lock PID is dead
###############################################################################
log "=== Worktree cleanup ==="

removed_wt=0
skipped_wt=0

# Parse porcelain output to build (path -> lock_pid) map
declare -A WT_PID_MAP
current_wt=""
while IFS= read -r line; do
  if [[ "$line" =~ ^worktree\ (.+)$ ]]; then
    current_wt="${BASH_REMATCH[1]}"
  elif [[ "$line" =~ ^locked.*\(pid\ ([0-9]+)\)$ ]]; then
    WT_PID_MAP["$current_wt"]="${BASH_REMATCH[1]}"
  fi
done < <(git -C "$REPO_ROOT" worktree list --porcelain 2>&1)

for wt_path in "${!WT_PID_MAP[@]}"; do
  pid="${WT_PID_MAP[$wt_path]}"

  # Never touch the main worktree or our own worktree
  if [[ "$wt_path" == "$REPO_ROOT" ]]; then
    continue
  fi
  if [[ "$wt_path" == "$(pwd -P 2>/dev/null || pwd)" ]]; then
    log "  SKIP (self): $(basename "$wt_path")"
    continue
  fi
  # Only auto-remove "agent-*" worktrees to stay conservative
  if [[ "$(basename "$wt_path")" != agent-* ]]; then
    info "  SKIP (non-agent worktree, manual review needed): $(basename "$wt_path")"
    continue
  fi

  if kill -0 "$pid" 2>/dev/null; then
    info "  ALIVE (pid $pid): $(basename "$wt_path")"
    skipped_wt=$((skipped_wt + 1))
  else
    log "  REMOVE (dead pid $pid): $(basename "$wt_path")"
    if ! $DRY_RUN; then
      git -C "$REPO_ROOT" worktree unlock "$wt_path" 2>/dev/null || true
      if git -C "$REPO_ROOT" worktree remove --force "$wt_path" 2>&1; then
        removed_wt=$((removed_wt + 1))
      else
        echo "  WARNING: failed to remove $wt_path"
      fi
    else
      echo "[DRY-RUN] would remove: $wt_path"
      removed_wt=$((removed_wt + 1))
    fi
  fi
done

log "  Worktrees: removed=$removed_wt, skipped-alive=$skipped_wt"

###############################################################################
# 2. Stash cleanup: drop stashes on master / no-branch / still-existing branches
###############################################################################
log "=== Stash cleanup ==="

dropped_stashes=0
kept_stashes=0

# Build set of local branch names
declare -A LOCAL_BRANCHES
while IFS= read -r branch; do
  LOCAL_BRANCHES["$branch"]=1
done < <(git -C "$REPO_ROOT" branch --format='%(refname:short)' 2>/dev/null)

# Process stashes in reverse order so indices stay valid after drops
mapfile -t STASH_LINES < <(git -C "$REPO_ROOT" stash list 2>/dev/null)
total=${#STASH_LINES[@]}

for ((i = total - 1; i >= 0; i--)); do
  line="${STASH_LINES[$i]}"

  # Extract branch from "stash@{N}: WIP on <branch>: ..." or "stash@{N}: On <branch>: ..."
  if [[ "$line" =~ stash@\{[0-9]+\}:\ (WIP\ on|On)\ ([^:]+): ]]; then
    branch="${BASH_REMATCH[2]}"
  else
    continue
  fi

  reason=""
  if [[ "$branch" == "master" ]]; then
    reason="on master (work preserved in commits)"
  elif [[ "$branch" == "(no branch)" ]]; then
    reason="detached HEAD ephemeral state"
  elif [[ -v "LOCAL_BRANCHES[$branch]" ]]; then
    reason="branch still exists locally (stash is redundant)"
  fi

  if [[ -n "$reason" ]]; then
    log "  DROP stash@{$i} [$branch] — $reason"
    if ! $DRY_RUN; then
      git -C "$REPO_ROOT" stash drop "stash@{$i}" 2>/dev/null &&
        dropped_stashes=$((dropped_stashes + 1)) || true
    else
      echo "[DRY-RUN] would drop stash@{$i}: $line"
      dropped_stashes=$((dropped_stashes + 1))
    fi
  else
    kept_stashes=$((kept_stashes + 1))
  fi
done

log "  Stashes: dropped=$dropped_stashes, kept=$kept_stashes"

###############################################################################
# 3. Summary
###############################################################################
log "=== Done ==="
log "  Worktrees remaining: $(git -C "$REPO_ROOT" worktree list 2>/dev/null | wc -l)"
log "  Stashes remaining:   $(git -C "$REPO_ROOT" stash list 2>/dev/null | wc -l)"
