#!/usr/bin/env bash
#
# bisect-common.sh — shared helpers for /bisect-regression and /bisect-model-quality.
#
# Sourced (NOT executed) by the two bisect skills. Provides:
#   - structured-log helpers (bisect_log, bisect_warn, bisect_die)
#   - pre-flight + post-flight guards that stash/restore uncommitted changes so
#     a bisect run never silently consumes the operator's working tree
#   - markdown verdict rendering used by both skills' final output
#   - exit-code constants matching `git bisect run` conventions
#
# Both /bisect-regression (code commits, O(N) over a sha range) and
# /bisect-model-quality (ONNX checkpoints, O(N) over an ordered model list)
# need the same wrappers; before consolidation each skill grew its own copy
# with subtle divergences. This file is the single source of truth.

# shellcheck shell=bash
set -euo pipefail

# Exit codes — match `git bisect run` semantics. Exported so driver scripts
# that source this library can reference them inside heredocs / nested shells.
# 0   → commit/checkpoint is GOOD
# 1   → commit/checkpoint is BAD
# 125 → SKIP (build broken for unrelated reasons; cannot decide)
# shellcheck disable=SC2034  # consumed by sourcing driver scripts
export BISECT_GOOD=0
# shellcheck disable=SC2034
export BISECT_BAD=1
# shellcheck disable=SC2034
export BISECT_SKIP=125

# bisect_log <msg…> — informational line on stderr; safe inside `git bisect run`.
bisect_log() {
  printf '[bisect] %s\n' "$*" >&2
}

# bisect_warn <msg…> — yellow on TTY, prefixed on stderr.
bisect_warn() {
  if [[ -t 2 ]]; then
    printf '\033[33m[bisect][warn] %s\033[0m\n' "$*" >&2
  else
    printf '[bisect][warn] %s\n' "$*" >&2
  fi
}

# bisect_die <msg…> — fatal; non-zero exit. Uses exit code 2 (operator error,
# distinct from the 0/1/125 verdict codes consumed by git bisect run).
bisect_die() {
  if [[ -t 2 ]]; then
    printf '\033[31m[bisect][error] %s\033[0m\n' "$*" >&2
  else
    printf '[bisect][error] %s\n' "$*" >&2
  fi
  exit 2
}

# bisect_require_clean_tree — refuse to start unless the index + worktree are
# clean. Operators should commit or stash before launching a bisect so the
# checkout sequence does not silently overwrite their work. Used by both
# /bisect-regression (which rewrites HEAD on every step) and
# /bisect-model-quality (which only reads, but checks the same gate for
# operator predictability).
bisect_require_clean_tree() {
  local repo_root
  repo_root=$(git rev-parse --show-toplevel) || bisect_die "not inside a git repo"
  if ! git -C "$repo_root" diff --quiet; then
    bisect_die "working tree has unstaged changes; commit or stash before bisecting"
  fi
  if ! git -C "$repo_root" diff --cached --quiet; then
    bisect_die "working tree has staged changes; commit or stash before bisecting"
  fi
}

# bisect_stash_push — stash any uncommitted changes (including untracked) and
# echo the stash ref. The matching bisect_stash_pop must run in a trap so the
# operator's work is restored even on abnormal exit.
#
# Usage:
#   stash_ref=$(bisect_stash_push) || true
#   trap 'bisect_stash_pop "$stash_ref"' EXIT
bisect_stash_push() {
  local repo_root msg
  repo_root=$(git rev-parse --show-toplevel) || bisect_die "not inside a git repo"
  msg="bisect-common-autostash-$(date +%s)"
  if git -C "$repo_root" stash push -u -m "$msg" >/dev/null 2>&1; then
    printf '%s' "$msg"
  else
    # Nothing to stash — emit empty marker so the matching pop is a no-op.
    printf ''
  fi
}

# bisect_stash_pop <stash_ref> — restore a previously pushed stash. Safe to call
# with an empty argument (no-op). Logs a warning instead of failing if the
# stash list no longer contains the reference (operator may have already
# restored manually).
bisect_stash_pop() {
  local stash_ref="${1:-}" repo_root
  [[ -z "$stash_ref" ]] && return 0
  repo_root=$(git rev-parse --show-toplevel) || return 0
  local hit
  hit=$(git -C "$repo_root" stash list | grep -F "$stash_ref" | head -n1 | awk -F: '{print $1}') || true
  if [[ -z "$hit" ]]; then
    bisect_warn "auto-stash '$stash_ref' not found in stash list; nothing to pop"
    return 0
  fi
  if ! git -C "$repo_root" stash pop --quiet "$hit"; then
    bisect_warn "failed to pop auto-stash '$hit'; restore manually with: git stash pop $hit"
  fi
}

# bisect_render_verdict <kind> <first_bad> <out_path> [<extra-md>] — write a
# Markdown summary used by both skills. <kind> is "commit" or "checkpoint".
# <first_bad> is the offending sha or model path. <out_path> is the file to
# write. Optional <extra-md> is appended after the standard table.
bisect_render_verdict() {
  local kind="${1:?kind required}"
  local first_bad="${2:?first_bad required}"
  local out_path="${3:?out_path required}"
  local extra_md="${4:-}"

  mkdir -p "$(dirname "$out_path")"
  {
    printf '# bisect verdict — first-bad %s\n\n' "$kind"
    printf '| field | value |\n'
    printf '|-------|-------|\n'
    printf '| kind  | %s |\n' "$kind"
    # shellcheck disable=SC2016  # backticks are literal markdown code-span markers
    printf '| first-bad | `%s` |\n' "$first_bad"
    printf '| produced  | %s |\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf '\n'
    if [[ -n "$extra_md" ]]; then
      printf '%s\n' "$extra_md"
    fi
  } >"$out_path"
  bisect_log "verdict written: $out_path"
}

# bisect_assert_two_revs <good> <bad> — sanity-check that two refs resolve to
# different commits in the same history. Catches operator typos before a long
# bisect run wastes wall time.
bisect_assert_two_revs() {
  local good="${1:?good rev required}"
  local bad="${2:?bad rev required}"
  local good_sha bad_sha
  good_sha=$(git rev-parse --verify "$good^{commit}" 2>/dev/null) ||
    bisect_die "good rev '$good' does not resolve to a commit"
  bad_sha=$(git rev-parse --verify "$bad^{commit}" 2>/dev/null) ||
    bisect_die "bad rev '$bad' does not resolve to a commit"
  if [[ "$good_sha" == "$bad_sha" ]]; then
    bisect_die "good and bad refs resolve to the same commit ($good_sha); nothing to bisect"
  fi
  if ! git merge-base --is-ancestor "$good_sha" "$bad_sha" 2>/dev/null; then
    bisect_die "good ($good_sha) is not an ancestor of bad ($bad_sha); cannot bisect"
  fi
}
