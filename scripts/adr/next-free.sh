#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# scripts/adr/next-free.sh — print (or atomically claim) the next available ADR number.
#
# Accounts for:
#   - ADR files already present in the local working tree
#   - ADR stub files (.md.stub) created by prior --claim calls in this session
#   - ADR files already merged into origin/master (cross-branch awareness)
#   - In-flight branches on origin that contain an ADR file at their tip
#     (fetched via git ls-remote + batch git ls-tree — < 5 s for 600+ ADRs /
#      30+ branches)
#   - ADR number claims written by sibling worktrees sharing the same .git/
#     (via .git/adr-claims/<NUMBER> side-files)
#
# Usage:
#   scripts/adr/next-free.sh
#   # -> e.g. "0532"   (read-only; prints next free number, does not claim)
#
#   scripts/adr/next-free.sh --claim <topic-slug>
#   # -> e.g. "0532"   (atomically creates docs/adr/0532-<topic-slug>.md.stub
#   #                   AND .git/adr-claims/0532 side-pointer)
#   #                   Prints the reserved number.
#   #                   Parallel callers in ANY worktree sharing this .git/ observe
#   #                   the claim and skip that number.
#   #                   Rename .stub -> .md when committing the real ADR.
#
#   scripts/adr/next-free.sh --release <NNNN>
#   # -> releases a previously claimed stub (e.g. if the PR is abandoned).
#   #    Also removes the matching .git/adr-claims/<NNNN> file.
#
# Atomic claim guarantee (single machine, parallel callers):
#   Claims use a per-repo lock dir under /tmp (POSIX mkdir is atomic on
#   Linux ext4/tmpfs) to serialise concurrent invocations on the same host.
#   The stub file is the persistent cross-process signal; after the lock is
#   released the next caller sees the stub and skips the number.
#   The .git/adr-claims/<NUMBER> side-file is the cross-worktree signal;
#   sibling worktrees (agents working in parallel) see it even if they have
#   not yet fetched the stub branch.
#
# Remote-branch awareness:
#   With --claim, the script fetches all origin branches (git ls-remote) and
#   batch-queries their docs/adr/ trees (git ls-tree --batch-all-objects or
#   per-SHA via a single git cat-file --batch run).  Numbers claimed by
#   in-flight branches are skipped.  This covers the parallel-agent scenario
#   where each agent pushes to its own branch before merging.
#
# Offline / network-fail mode:
#   If git ls-remote or gh pr list fail (offline, no network, timeout), the
#   script logs a WARNING to stderr and falls back to local-only behavior.
#   The number allocated is still collision-free among local + master state,
#   but may collide with another in-flight agent on a different machine.
#
# Performance:
#   Target: < 5 s on a repo with 600+ ADRs and 30+ open branches.
#   Strategy: one git ls-remote call; one git fetch; N git ls-tree calls
#   (one per branch) are replaced by a single git cat-file --batch run over
#   the set of branch tip SHAs, then grep for docs/adr/ entries.
#
# See ADR-0386 (docs/adr/0386-adr-numbering-collision-prevention.md),
#     ADR-0535 (docs/adr/0535-adr-atomic-allocator.md), and
#     ADR-0628 (docs/adr/0628-adr-allocator-remote-aware.md) — the ADR for
#     this remote-aware extension.

set -euo pipefail

# ── shallow-safe fetch depth ──────────────────────────────────────────────────
#
# `--depth` CONVERTS a full clone into a shallow one: it writes `.git/shallow`,
# after which `git merge-base` finds nothing and every `git rebase` in the
# repository — including in all of its worktrees, which share one `.git` —
# reports the whole tree as conflicting.  This script runs on every ADR claim,
# so an unguarded `--depth` silently corrupts a developer's clone several times
# an hour.  CI checkouts are already shallow and genuinely benefit from the
# smaller fetch, so keep the depth flags there and drop them everywhere else.
adr_depth_args() {
  if [ "$(git rev-parse --is-shallow-repository 2>/dev/null)" = "true" ]; then
    printf '%s\n' "$@"
  fi
}

# ── constants ────────────────────────────────────────────────────────────────

REPO_ROOT="$(git rev-parse --show-toplevel)"
# Use the common .git dir (resolves correctly even inside a worktree), made
# absolute: `git rev-parse --git-common-dir` prints `.git` at the root, `../.git`
# from docs/ and an absolute path in a linked worktree, and the lock key below
# must not depend on the caller's working directory.
GIT_COMMON_DIR="$(cd "$(git rev-parse --git-common-dir)" && pwd -P)"
ADR_DIR="${REPO_ROOT}/docs/adr"
# Cross-worktree claim signals live in the common .git dir so all worktrees
# sharing this repo see them regardless of which branch they have checked out.
ADR_CLAIMS_DIR="${GIT_COMMON_DIR}/adr-claims"

# Lock file is keyed by the common git dir so parallel agents in any worktree
# of the same repo contend on the same lock, but separate repos on the same
# host do not.
_git_key="$(printf '%s' "${GIT_COMMON_DIR}" | tr '/' '_')"
LOCK_DIR="/tmp/vmaf_adr_claim_lock_${_git_key}"
# Identifies this process's hold on the lock; release removes only its own.
LOCK_TOKEN="$$-${RANDOM}${RANDOM}-$(date +%s%N)"
# Upper bound on numbers tried past the first free one when a claim file of
# the same number already exists.
CLAIM_RETRIES=100

# ── helpers ───────────────────────────────────────────────────────────────────

usage() {
  printf 'Usage:\n' >&2
  printf '  scripts/adr/next-free.sh                   # print next free number (read-only)\n' >&2
  printf '  scripts/adr/next-free.sh --claim <slug>    # atomically reserve next number\n' >&2
  printf '  scripts/adr/next-free.sh --release <NNNN>  # release a previously claimed stub\n' >&2
  exit 1
}

# Seconds a claim waits for the lock (VMAF_ADR_LOCK_WAIT_S, default 60).
LOCK_WAIT_S="${VMAF_ADR_LOCK_WAIT_S:-60}"

# The lock is held by whoever creates ${LOCK_DIR}/owner (O_EXCL through
# noclobber); the directory only holds that file. A directory without an owner
# file, as a claim killed between mkdir and the owner write leaves it, is
# therefore free: the next caller creates the owner file in it (issue #2784).

# Print the process id of the lock's holder, or nothing when there is none.
_lock_owner_pid() {
  local token=""
  if [ -f "${LOCK_DIR}/owner" ]; then
    token="$(cut -d' ' -f1 "${LOCK_DIR}/owner" 2>/dev/null)" || token=""
  fi
  printf '%s' "${token%%-*}"
}

# Succeed when process $1 is running (any user; `kill -0` fails on another
# user's process).
_pid_alive() {
  ps -p "$1" >/dev/null 2>&1
}

# Create the owner file; succeed only for the caller that created it.
_try_lock() {
  mkdir "${LOCK_DIR}" 2>/dev/null || [ -d "${LOCK_DIR}" ] || return 1
  (
    set -o noclobber
    printf '%s %s\n' "${LOCK_TOKEN}" "${REPO_ROOT}" >"${LOCK_DIR}/owner"
  ) 2>/dev/null
}

# Remove the owner file of a holder whose process is gone. Takeovers are
# serialised by a second directory; a takeover directory older than a minute
# was left by a killed takeover.
_take_over_stale_lock() {
  local mutex="${LOCK_DIR}.takeover" owner="" pid
  if ! mkdir "${mutex}" 2>/dev/null; then
    if [ -n "$(find "${mutex}" -prune -mmin +1 2>/dev/null)" ]; then
      rmdir "${mutex}" 2>/dev/null || printf 'note: takeover lock %s already gone\n' "${mutex}" >&2
    fi
    return 0
  fi
  owner="$(cat "${LOCK_DIR}/owner" 2>/dev/null)" || owner=""
  pid="${owner%% *}"
  pid="${pid%%-*}"
  # A holder may release and exit between the read and the check, and a new
  # holder write its own file: remove the file only if, read after the check,
  # it still holds the dead holder's token. Tokens are unique and a dead
  # process removes nothing, so that file is the dead holder's.
  if [ -n "${pid}" ] && ! _pid_alive "${pid}" &&
    [ "$(cat "${LOCK_DIR}/owner" 2>/dev/null)" = "${owner}" ]; then
    rm -f "${LOCK_DIR}/owner"
    printf 'WARNING: took over a stale ADR allocator lock (%s).\n' "${owner}" >&2
  fi
  rmdir "${mutex}" 2>/dev/null || printf 'note: takeover lock %s already gone\n' "${mutex}" >&2
}

# Acquire the per-repo lock. The owner file of a holder whose process is gone
# is taken over; otherwise the claim fails after LOCK_WAIT_S seconds.
_acquire_lock() {
  local deadline pid owner
  case "${LOCK_WAIT_S}" in
    '' | *[!0-9]*)
      printf 'ERROR: VMAF_ADR_LOCK_WAIT_S must be a whole number of seconds, got %s\n' "${LOCK_WAIT_S}" >&2
      exit 1
      ;;
  esac
  deadline=$(($(date +%s) + LOCK_WAIT_S))
  # The trap removes only a lock holding this process's token.
  trap '_release_lock' EXIT INT TERM
  while ! _try_lock; do
    pid="$(_lock_owner_pid)"
    owner=""
    if [ -f "${LOCK_DIR}/owner" ]; then
      owner="$(cat "${LOCK_DIR}/owner" 2>/dev/null)" || owner=""
    fi
    if [ -n "${pid}" ] && ! _pid_alive "${pid}"; then
      _take_over_stale_lock
    fi
    if [ "$(date +%s)" -ge "${deadline}" ]; then
      printf 'ERROR: could not acquire ADR allocator lock (%s) within %s s.\n' "${LOCK_DIR}" "${LOCK_WAIT_S}" >&2
      printf '       Holder: %s (process %s).\n' "${owner:-no owner file}" "${pid:-unknown}" >&2
      printf '       Raise VMAF_ADR_LOCK_WAIT_S if the holder is a slow claim.\n' >&2
      exit 1
    fi
    sleep 0.1
  done
}

# Succeed for grep's "no match" status (1); return any other failure status.
# Use as: `grep ... || _no_match_ok $?`.
_no_match_ok() {
  [ "$1" -le 1 ] || return "$1"
}

# Remove the lock only while this process owns it; another holder's lock stays.
# The directory goes only when empty: a waiter may already have created its
# owner file in it.
_release_lock() {
  local owner=""
  if [ -f "${LOCK_DIR}/owner" ]; then
    owner="$(cut -d' ' -f1 "${LOCK_DIR}/owner" 2>/dev/null)" || owner=""
  fi
  if [ "${owner}" = "${LOCK_TOKEN}" ]; then
    rm -f "${LOCK_DIR}/owner"
    rmdir "${LOCK_DIR}" 2>/dev/null || [ -d "${LOCK_DIR}" ] ||
      printf 'WARNING: could not remove ADR allocator lock %s\n' "${LOCK_DIR}" >&2
  fi
  trap - EXIT INT TERM
}

# Collect ADR numbers from origin's remote branches by batch-querying
# all branch tip SHAs for docs/adr/ entries in a single git ls-tree pass.
# Prints one 4-digit number per line; is best-effort (soft failure on network
# outage or offline dev).
#
# Offline detection: the caller (_ls_remote_heads in the main shell) checks the
# exit status of git ls-remote and sets REMOTE_OFFLINE in the MAIN shell before
# calling this function.  This function MUST NOT set REMOTE_OFFLINE — it runs
# inside a process substitution subshell so variable assignments would be lost.
REMOTE_OFFLINE=0

# _ls_remote_heads: emit raw git ls-remote --heads output.  Separate function
# so the caller can capture it in the main shell (not a subshell) before piping.
_ls_remote_heads() {
  git ls-remote --heads origin 2>/dev/null
}

# _collect_remote_branch_numbers: internal worker; called from a process
# substitution.  Receives the raw ls-remote output on stdin (piped from the
# main shell) so it never calls git ls-remote itself.
_collect_remote_branch_numbers() {
  local ls_remote_out="$1"

  if [ -z "${ls_remote_out}" ]; then
    return 0
  fi

  # Step 2: extract unique SHAs (multiple refs can share the same commit).
  # Skip master — it's already covered by _collect_local_taken via origin/master.
  local -a shas=()
  while IFS=' 	' read -r sha ref; do
    [ -n "${sha}" ] || continue
    [[ "${ref}" == "refs/heads/master" ]] && continue
    shas+=("${sha}")
  done < <(printf '%s\n' "${ls_remote_out}" | LC_ALL=C sort -u -k1,1)

  if [ "${#shas[@]}" -eq 0 ]; then
    return 0
  fi

  # Step 3: fetch all branch tips so git ls-tree can resolve the SHAs.
  # --no-tags avoids tag-name expansion; --depth=1 is the minimal fetch.
  # Soft failure: if the network drops between the ls-remote above and here,
  # ls-tree will simply find nothing.
  mapfile -t _adr_depth < <(adr_depth_args --depth=1)
  if ! git fetch --no-tags "${_adr_depth[@]}" --quiet origin \
    "${shas[@]}" 2>/dev/null; then
    printf 'WARNING: could not fetch remote branch tips; their ADR numbers are skipped.\n' >&2
  fi

  # Step 4: for each SHA, run git ls-tree to enumerate docs/adr/ entries.
  # git ls-tree is fast (tree object lookup, no working-tree I/O).
  for sha in "${shas[@]}"; do
    local listing
    if listing="$(git ls-tree -r --name-only "${sha}" -- docs/adr/ 2>/dev/null)"; then
      printf '%s\n' "${listing}" |
        grep -oE 'docs/adr/[0-9]{4}-' |
        grep -oE '[0-9]{4}' || _no_match_ok $?
    fi
  done
}

# Collect all taken 4-digit ADR numbers from:
#   1. Local real ADR files in docs/adr/
#   2. Local stub files (.md.stub)
#   3. origin/master
#   4. .git/adr-claims/ (cross-worktree claims by sibling agents)
# Prints one 4-digit number per line, sorted unique.
# Print each argument that names an existing path (a glob that matched nothing
# stays a literal pattern and is skipped).
_list_existing() {
  local path
  for path in "$@"; do
    if [ -e "${path}" ] || [ -L "${path}" ]; then
      printf '%s\n' "${path}"
    fi
  done
}

_collect_local_taken() {
  {
    # Local real ADR files
    _list_existing "${ADR_DIR}"/[0-9][0-9][0-9][0-9]-*.md
    # Local stub files (cross-process claim markers within this worktree)
    _list_existing "${ADR_DIR}"/[0-9][0-9][0-9][0-9]-*.md.stub
    # origin/master (already fetched before this function is called)
    local master_listing
    if master_listing="$(git ls-tree -r --name-only origin/master -- docs/adr/ 2>/dev/null)"; then
      printf '%s\n' "${master_listing}" |
        grep -E '^docs/adr/[0-9]{4}-' || _no_match_ok $?
    fi
    # .git/adr-claims/ — claims by sibling worktrees sharing this .git/
    if [ -d "${ADR_CLAIMS_DIR}" ]; then
      _list_existing "${ADR_CLAIMS_DIR}"/[0-9][0-9][0-9][0-9] | sed 's|.*/||'
    fi
  } |
    sed 's|.*/||' |
    grep -oE '^[0-9]{4}' |
    sort -u
}

# Given a sorted unique list of taken numbers on stdin, print the next free one.
# Always returns max(taken)+1 rather than filling gaps, so a claim that has
# not yet pushed is still safe (the gap is never reused).
_next_free_from_taken() {
  local -a taken=()
  while IFS= read -r n; do
    taken+=("${n}")
  done
  if [ "${#taken[@]}" -eq 0 ]; then
    printf '0001\n'
    return
  fi
  # Highest taken + 1.  IDs are never reused (per ADR-0386).
  local highest="${taken[-1]}"
  printf '%04d\n' $((10#${highest} + 1))
}

# Write the stub frontmatter for a newly claimed ADR.
_write_stub() {
  local stub_path="$1" number="$2" slug="$3"
  local ts dt
  ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  dt="$(date -u +%Y-%m-%d)"
  {
    printf '<!-- ADR-%s stub — claimed by scripts/adr/next-free.sh --claim %s on %s -->\n' \
      "${number}" "${slug}" "${ts}"
    printf '<!-- Replace this file with the real ADR and rename to %s-%s.md before committing. -->\n' \
      "${number}" "${slug}"
    printf '<!-- To abandon this claim run: scripts/adr/next-free.sh --release %s -->\n' \
      "${number}"
    printf '\n# ADR-%s: <fill in title>\n\n' "${number}"
    printf -- '- **Status**: Proposed\n'
    printf -- '- **Date**: %s\n' "${dt}"
    printf -- '- **Deciders**: <fill in>\n'
    printf -- '- **Tags**: <fill in>\n'
    printf '\n## Context\n\n<fill in>\n\n'
    printf '## Decision\n\n<fill in>\n\n'
    printf '## Alternatives considered\n\n'
    printf '| Option | Pros | Cons | Why not chosen |\n'
    printf '|---|---|---|---|\n'
    printf '| | | | |\n\n'
    printf '## Consequences\n\n'
    printf -- '- **Positive**: <fill in>\n'
    printf -- '- **Negative**: <fill in>\n'
    printf -- '- **Neutral / follow-ups**: <fill in>\n\n'
    printf '## References\n\n'
    printf -- '- See [ADR-0535](0535-adr-atomic-allocator.md) for the original allocator design.\n'
    printf -- '- See [ADR-0628](0628-adr-allocator-remote-aware.md) for the remote-aware extension.\n'
    printf -- '- Source: <req or Q<round>.<q>>\n'
  } >"${stub_path}"
}

# Create the .git/adr-claims/<NUMBER> side-pointer exclusively (O_CREAT|O_EXCL
# through noclobber); fails when the number is already claimed. This file is
# visible to ALL worktrees sharing this .git/ directory, and its exclusive
# creation decides a number even if two callers ever hold the lock at once.
_write_claim_sidepointer() {
  local number="$1" slug="$2"
  local ts branch
  ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  branch="$(git symbolic-ref --short HEAD 2>/dev/null || printf 'detached')"
  mkdir -p "${ADR_CLAIMS_DIR}"
  (
    set -o noclobber
    printf '%s %s %s\n' "${slug}" "${ts}" "${branch}" >"${ADR_CLAIMS_DIR}/${number}"
  ) 2>/dev/null
}

# Remove the .git/adr-claims/<NUMBER> side-pointer (called by --release).
_remove_claim_sidepointer() {
  local number="$1"
  if ! rm -f "${ADR_CLAIMS_DIR}/${number}" 2>/dev/null; then
    printf 'WARNING: could not remove claim pointer %s/%s\n' "${ADR_CLAIMS_DIR}" "${number}" >&2
  fi
}

# ── argument parsing ──────────────────────────────────────────────────────────

MODE="query"
SLUG=""
RELEASE_NUM=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --claim)
      MODE="claim"
      shift
      if [ "$#" -eq 0 ] || [[ "$1" == --* ]]; then
        printf 'ERROR: --claim requires a topic-slug argument (e.g. my-adr-topic).\n' >&2
        usage
      fi
      SLUG="$1"
      shift
      ;;
    --release)
      MODE="release"
      shift
      if [ "$#" -eq 0 ] || [[ "$1" == --* ]]; then
        printf 'ERROR: --release requires a 4-digit ADR number (e.g. 0532).\n' >&2
        usage
      fi
      RELEASE_NUM="$1"
      shift
      ;;
    -h | --help)
      usage
      ;;
    *)
      printf 'ERROR: unknown argument: %s\n' "$1" >&2
      usage
      ;;
  esac
done

cd "${REPO_ROOT}"

# ── release mode ──────────────────────────────────────────────────────────────

if [ "${MODE}" = "release" ]; then
  found=()
  while IFS= read -r f; do
    found+=("${f}")
  done < <(find "${ADR_DIR}" -maxdepth 1 -name "${RELEASE_NUM}-*.md.stub" 2>/dev/null | LC_ALL=C sort)
  if [ "${#found[@]}" -eq 0 ]; then
    printf 'ERROR: no stub file for %s found in %s.\n' "${RELEASE_NUM}" "${ADR_DIR}" >&2
    exit 1
  fi
  for f in "${found[@]}"; do
    rm -f "${f}"
    printf 'released %s (removed %s)\n' "${RELEASE_NUM}" "${f}" >&2
  done
  # Also remove the cross-worktree side-pointer if it exists.
  _remove_claim_sidepointer "${RELEASE_NUM}"
  printf 'released %s\n' "${RELEASE_NUM}"
  exit 0
fi

# ── shared network fetch (query and claim modes) ──────────────────────────────

# Fetch the latest master tip (soft failure on network outage or offline dev).
mapfile -t _adr_depth_master < <(adr_depth_args --depth=50)
git fetch origin master "${_adr_depth_master[@]}" --quiet 2>/dev/null || {
  REMOTE_OFFLINE=1
}

# ── query mode (read-only) ────────────────────────────────────────────────────

if [ "${MODE}" = "query" ]; then
  # Query mode: local + master + claims; no remote-branch scan (read-only,
  # fast path).
  _collect_local_taken | _next_free_from_taken
  if [ "${REMOTE_OFFLINE}" -eq 1 ]; then
    printf 'WARNING: could not reach origin — remote branch scan skipped.\n' >&2
    printf '         Collision risk if other agents are working on separate branches.\n' >&2
  fi
  exit 0
fi

# ── claim mode (atomic) ───────────────────────────────────────────────────────

# Validate slug before taking the lock.
if ! printf '%s' "${SLUG}" | grep -qE '^[a-z0-9][a-z0-9-]*$'; then
  printf 'ERROR: slug must match [a-z0-9][a-z0-9-]* (got: %s).\n' "${SLUG}" >&2
  exit 1
fi

# Scan remote branches for in-flight ADR numbers (best-effort; network-fail safe).
# This runs OUTSIDE the lock to avoid holding it during slow network I/O.
#
# _ls_remote_heads is called HERE in the main shell so we can capture its exit
# status in REMOTE_OFFLINE without the variable being swallowed by a subshell.
ls_remote_out=""
if ! ls_remote_out="$(_ls_remote_heads 2>/dev/null)"; then
  REMOTE_OFFLINE=1
fi

remote_numbers=()
if [ "${REMOTE_OFFLINE}" -eq 0 ]; then
  while IFS= read -r n; do
    [ -n "${n}" ] && remote_numbers+=("${n}")
  done < <(_collect_remote_branch_numbers "${ls_remote_out}" 2>/dev/null ||
    printf 'WARNING: remote branch scan failed.\n' >&2)
fi

if [ "${REMOTE_OFFLINE}" -eq 1 ]; then
  printf 'WARNING: could not reach origin — remote branch scan skipped.\n' >&2
  printf '         Collision risk if other agents are working on separate branches.\n' >&2
  printf '         Proceeding with local + master + .git/adr-claims/ only.\n' >&2
fi

# Acquire the exclusive per-repo lock (covers all worktrees sharing this .git/).
_acquire_lock

# Re-collect inside the lock so we see any claim written since we started.
# Merge local + remote branch numbers into a unified sorted-unique set.
number="$(
  {
    _collect_local_taken
    printf '%s\n' "${remote_numbers[@]+"${remote_numbers[@]}"}"
  } | LC_ALL=C sort -u | _next_free_from_taken
)"

# Take the number by creating its claim file exclusively; a number whose claim
# file exists already belongs to another caller, so move to the next one.
tries=0
while ! _write_claim_sidepointer "${number}" "${SLUG}"; do
  tries=$((tries + 1))
  if [ "${tries}" -ge "${CLAIM_RETRIES}" ]; then
    printf 'ERROR: no free ADR number within %s of the first candidate (claims in %s).\n' \
      "${CLAIM_RETRIES}" "${ADR_CLAIMS_DIR}" >&2
    exit 1
  fi
  number="$(printf '%04d' $((10#${number} + 1)))"
done

stub_path="${ADR_DIR}/${number}-${SLUG}.md.stub"

# The claim file is written BEFORE the lock is released, so a sibling agent
# that acquires the lock immediately after sees the claim.
_write_stub "${stub_path}" "${number}" "${SLUG}"

# Release the lock early to minimise critical-section duration.
_release_lock

printf '%s\n' "${number}"
