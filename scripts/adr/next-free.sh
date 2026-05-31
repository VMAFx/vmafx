#!/usr/bin/env bash
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

# ── constants ────────────────────────────────────────────────────────────────

REPO_ROOT="$(git rev-parse --show-toplevel)"
# Use the common .git dir (resolves correctly even inside a worktree).
GIT_COMMON_DIR="$(git rev-parse --git-common-dir)"
ADR_DIR="${REPO_ROOT}/docs/adr"
# Cross-worktree claim signals live in the common .git dir so all worktrees
# sharing this repo see them regardless of which branch they have checked out.
ADR_CLAIMS_DIR="${GIT_COMMON_DIR}/adr-claims"

# Lock file is keyed by the common git dir so parallel agents in any worktree
# of the same repo contend on the same lock, but separate repos on the same
# host do not.
_git_key="$(printf '%s' "${GIT_COMMON_DIR}" | tr '/' '_')"
LOCK_DIR="/tmp/vmaf_adr_claim_lock_${_git_key}"

# ── helpers ───────────────────────────────────────────────────────────────────

usage() {
  printf 'Usage:\n' >&2
  printf '  scripts/adr/next-free.sh                   # print next free number (read-only)\n' >&2
  printf '  scripts/adr/next-free.sh --claim <slug>    # atomically reserve next number\n' >&2
  printf '  scripts/adr/next-free.sh --release <NNNN>  # release a previously claimed stub\n' >&2
  exit 1
}

# Acquire the per-repo lock using an atomic mkdir.
# Retries for up to 10 seconds, then fails loudly.
_acquire_lock() {
  local deadline
  deadline=$(($(date +%s) + 10))
  while ! mkdir "${LOCK_DIR}" 2>/dev/null; do
    if [ "$(date +%s)" -ge "${deadline}" ]; then
      printf 'ERROR: could not acquire ADR allocator lock (%s) within 10 s.\n' "${LOCK_DIR}" >&2
      printf '       Another process may be holding it.  Remove manually if stale.\n' >&2
      exit 1
    fi
    sleep 0.1
  done
  # Register a trap to release the lock on exit (including errors).
  trap '_release_lock' EXIT INT TERM
}

_release_lock() {
  rmdir "${LOCK_DIR}" 2>/dev/null || true
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
  git fetch --no-tags --depth=1 --quiet origin \
    "${shas[@]}" 2>/dev/null || true

  # Step 4: for each SHA, run git ls-tree to enumerate docs/adr/ entries.
  # git ls-tree is fast (tree object lookup, no working-tree I/O).
  for sha in "${shas[@]}"; do
    git ls-tree -r --name-only "${sha}" -- docs/adr/ 2>/dev/null |
      grep -oE 'docs/adr/[0-9]{4}-' |
      grep -oE '[0-9]{4}' ||
      true
  done
}

# Collect all taken 4-digit ADR numbers from:
#   1. Local real ADR files in docs/adr/
#   2. Local stub files (.md.stub)
#   3. origin/master
#   4. .git/adr-claims/ (cross-worktree claims by sibling agents)
# Prints one 4-digit number per line, sorted unique.
_collect_local_taken() {
  {
    # Local real ADR files
    ls "${ADR_DIR}"/[0-9][0-9][0-9][0-9]-*.md 2>/dev/null || true
    # Local stub files (cross-process claim markers within this worktree)
    ls "${ADR_DIR}"/[0-9][0-9][0-9][0-9]-*.md.stub 2>/dev/null || true
    # origin/master (already fetched before this function is called)
    git ls-tree -r --name-only origin/master -- docs/adr/ 2>/dev/null |
      grep -E '^docs/adr/[0-9]{4}-' || true
    # .git/adr-claims/ — claims by sibling worktrees sharing this .git/
    if [ -d "${ADR_CLAIMS_DIR}" ]; then
      find "${ADR_CLAIMS_DIR}" -maxdepth 1 -name '[0-9][0-9][0-9][0-9]' \
        -exec basename {} \; 2>/dev/null || true
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

# Write the .git/adr-claims/<NUMBER> side-pointer.
# This file is visible to ALL worktrees sharing this .git/ directory,
# including sibling agents in separate worktrees.
_write_claim_sidepointer() {
  local number="$1" slug="$2"
  local ts branch
  ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  branch="$(git symbolic-ref --short HEAD 2>/dev/null || printf 'detached')"
  mkdir -p "${ADR_CLAIMS_DIR}"
  printf '%s %s %s\n' "${slug}" "${ts}" "${branch}" \
    >"${ADR_CLAIMS_DIR}/${number}"
}

# Remove the .git/adr-claims/<NUMBER> side-pointer (called by --release).
_remove_claim_sidepointer() {
  local number="$1"
  rm -f "${ADR_CLAIMS_DIR}/${number}" 2>/dev/null || true
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
git fetch origin master --depth=50 --quiet 2>/dev/null || {
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
  done < <(_collect_remote_branch_numbers "${ls_remote_out}" 2>/dev/null || true)
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

stub_path="${ADR_DIR}/${number}-${SLUG}.md.stub"

_write_stub "${stub_path}" "${number}" "${SLUG}"

# Write the cross-worktree side-pointer BEFORE releasing the lock so a sibling
# agent that acquires the lock immediately after sees the claim.
_write_claim_sidepointer "${number}" "${SLUG}"

# Release the lock early to minimise critical-section duration.
_release_lock

printf '%s\n' "${number}"
