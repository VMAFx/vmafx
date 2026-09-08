#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# Inventory agent worktrees and stashes; remove only explicitly selected,
# idle, clean agent checkouts. See ADR-1239 and the operator guide at
# docs/development/agent-worktree-discipline.md.

set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: cleanup-agent-state.sh [--dry-run]
       cleanup-agent-state.sh --apply --worktree /absolute/registered/path [...]

The default and --dry-run only report. Repeat --worktree to select exact paths.
Apply retains branches and all stashes. It refuses dirty/untracked/ignored
files, live or unknown owners, unreferenced detached HEADs, and main/self
worktrees. Keep selected worktrees idle throughout the operation.
USAGE
}

APPLY=false
MODE=""
TARGETS=()
while (($#)); do
  case "$1" in
    --apply | --dry-run)
      [[ -z "$MODE" || "$MODE" == "$1" ]] || {
        echo 'error: --apply and --dry-run are mutually exclusive' >&2
        exit 64
      }
      MODE="$1"
      [[ "$1" != --apply ]] || APPLY=true
      shift
      ;;
    --worktree)
      [[ $# -ge 2 && "$2" == /* ]] || {
        echo 'error: --worktree requires an absolute registered path' >&2
        exit 64
      }
      TARGETS+=("$2")
      shift 2
      ;;
    --help | -h)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage >&2
      exit 64
      ;;
  esac
done
if $APPLY && ((${#TARGETS[@]} == 0)); then
  echo 'error: --apply requires at least one explicit --worktree target' >&2
  exit 64
fi

REPO_ROOT=$(git rev-parse --show-toplevel)
INVENTORY=$(mktemp)
UNLOCKED_PATH=""
UNLOCKED_REASON=""
cleanup() {
  if [[ -n "$UNLOCKED_PATH" && -d "$UNLOCKED_PATH" ]]; then
    git -C "$REPO_ROOT" worktree lock --reason "$UNLOCKED_REASON" "$UNLOCKED_PATH" ||
      echo "warning: could not restore lock for $UNLOCKED_PATH" >&2
  fi
  rm -f -- "$INVENTORY"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

declare -A WT_HEAD=() WT_LOCK=()
WORKTREES=()
MAIN_WORKTREE=""
load_worktrees() {
  local field current=""
  WT_HEAD=() WT_LOCK=() WORKTREES=() MAIN_WORKTREE=""
  git -C "$REPO_ROOT" worktree list --porcelain -z >"$INVENTORY"
  while IFS= read -r -d '' field; do
    case "$field" in
      'worktree '*)
        current="${field#worktree }"
        WORKTREES+=("$current")
        [[ -n "$MAIN_WORKTREE" ]] || MAIN_WORKTREE="$current"
        ;;
      'HEAD '*) WT_HEAD["$current"]="${field#HEAD }" ;;
      'locked '*) WT_LOCK["$current"]="${field#locked }" ;;
    esac
  done <"$INVENTORY"
}

REASON=""
eligible() {
  local path="$1" pid status refs gitdir state ps_code head
  REASON='not a registered worktree'
  [[ -n "${WT_HEAD[$path]:-}" ]] || return 1
  REASON='main or current worktree'
  [[ "$path" != "$MAIN_WORKTREE" && "$path" != "$REPO_ROOT" ]] || return 1
  REASON='not an agent-* worktree'
  [[ "${path##*/}" == agent-* ]] || return 1
  REASON='missing directory or symlink; review ownership manually'
  [[ -d "$path" && ! -L "$path" ]] || return 1
  REASON='missing or unrecognized owner PID in lock reason'
  [[ "${WT_LOCK[$path]:-}" =~ \(pid\ ([1-9][0-9]*)\)$ ]] || return 1
  pid="${BASH_REMATCH[1]}"
  REASON="owner PID $pid is alive or cannot be checked"
  if kill -0 "$pid" 2>/dev/null; then return 1; fi
  if ps -p "$pid" -o pid= >/dev/null 2>&1; then return 1; else ps_code=$?; fi
  [[ "$ps_code" == 1 ]] || return 1
  REASON='could not inspect worktree state'
  gitdir=$(git -C "$path" rev-parse --absolute-git-dir) || return 1
  for state in index.lock MERGE_HEAD CHERRY_PICK_HEAD REVERT_HEAD BISECT_START rebase-merge rebase-apply; do
    REASON="operation in progress: $state"
    [[ ! -e "$gitdir/$state" ]] || return 1
  done
  REASON='could not inspect tracked, untracked and ignored files'
  status=$(git -C "$path" status --porcelain=v1 --untracked-files=all --ignored=matching) || return 1
  REASON='tracked changes, untracked files or ignored artifacts need review'
  [[ -z "$status" ]] || return 1
  REASON='HEAD changed during inspection; retry after its writer is idle'
  head=$(git -C "$path" rev-parse HEAD) || return 1
  [[ "$head" == "${WT_HEAD[$path]}" ]] || return 1
  REASON='HEAD has no preserving branch/tag ref, or reachability check failed'
  refs=$(git -C "$REPO_ROOT" for-each-ref --contains="${WT_HEAD[$path]}" --format='%(refname)') || return 1
  [[ -n "$refs" ]] || return 1
  REASON="clean checkout; recorded owner PID $pid has exited; refs retain HEAD"
}

load_worktrees
printf 'Worktrees (%s):\n' "${#WORKTREES[@]}"
for path in "${WORKTREES[@]}"; do
  if eligible "$path"; then
    printf 'REVIEW %q — %s\n' "$path" "$REASON"
  else
    printf 'KEEP   %q — %s\n' "$path" "$REASON"
  fi
done
printf '\nStashes (all retained; branch existence does not prove redundancy):\n'
git -C "$REPO_ROOT" stash list --format='%H %gd %s'

# Validate every selected target before changing any checkout. Recheck each
# immediately before removal; Git's non-force remove is the final guard.
declare -A SELECTED=()
for path in "${TARGETS[@]}"; do
  [[ -z "${SELECTED[$path]:-}" ]] || {
    echo "error: duplicate worktree target: $path" >&2
    exit 64
  }
  SELECTED["$path"]=1
  if ! eligible "$path"; then
    printf 'error: refusing %q: %s\n' "$path" "$REASON" >&2
    exit 1
  fi
done
if ! $APPLY; then
  printf '\nReport only; no worktrees, branches or stashes changed.\n'
  exit 0
fi
for path in "${TARGETS[@]}"; do
  load_worktrees
  if ! eligible "$path"; then
    printf 'error: target changed; refusing %q: %s\n' "$path" "$REASON" >&2
    exit 1
  fi
  UNLOCKED_PATH="$path"
  UNLOCKED_REASON="${WT_LOCK[$path]}"
  git -C "$REPO_ROOT" worktree unlock "$path"
  git -C "$REPO_ROOT" worktree remove "$path"
  UNLOCKED_PATH=""
  printf 'Removed selected checkout %q; branch refs retained.\n' "$path"
done
