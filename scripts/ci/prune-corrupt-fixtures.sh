#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Drop fixtures that a previous download left in an unusable state, so the
# lazy fetcher re-downloads them instead of trusting them.
#
# compat/python-vmaf/config.py::download_reactively fetches a fixture only when
# the local file is ABSENT. A file that exists but is empty, or that holds an
# HTTP error page or a Git-LFS pointer instead of the payload, is therefore
# never repaired: it is "present", so the download is skipped, and the test
# fails with something unhelpful like `no frames decoded`. When such a file is
# captured into the CI fixture cache, every later run on that branch restores
# it and fails the same way until someone deletes the cache by hand.
#
# The success()-gated cache save stops NEW poisoning; this prunes what is
# already out there. Deleting a fixture is always safe — the worst case is one
# extra download.
#
# The cache also holds the files of ROOT that git tracks, in the revision of
# the run that saved it, and actions/cache/restore writes them over the
# checkout. A restore-keys hit (any run whose test files differ) therefore
# brings back the old revision of a tracked fixture a later commit changed,
# and the tests read it instead of the commit's: a dataset file without the
# fields its test asserts, and the run fails, so the success()-gated save of
# a fresh key never happens either. After pruning, every tracked file under
# ROOT is put back to the checked-out revision; untracked downloads stay.
#
# Usage: prune-corrupt-fixtures.sh [--restore-tracked] [ROOT]
#        (default ROOT: python/test/resource; the CI workflows pass
#        --restore-tracked right after restoring the fixture cache)
set -euo pipefail
export LC_ALL=C

restore_tracked=0
if [ "${1:-}" = "--restore-tracked" ]; then
  restore_tracked=1
  shift
fi
root="${1:-python/test/resource}"

if [ ! -d "${root}" ]; then
  printf 'prune-corrupt-fixtures: %s does not exist, nothing to do\n' "${root}"
  exit 0
fi

pruned=0
scanned=0

# A fixture is suspect when it is empty, or when its first bytes are text that
# no binary fixture would start with. Checked against the raw bytes so a
# truncated YUV that merely happens to be short is left alone — only a
# recognisable non-payload is removed.
while IFS= read -r -d '' f; do
  scanned=$((scanned + 1))
  reason=""

  if [ ! -s "${f}" ]; then
    reason="empty"
  else
    head_bytes=$(head -c 64 "${f}" 2>/dev/null | tr -d '\0') || head_bytes=""
    case "${head_bytes}" in
      "version https://git-lfs"*)
        reason="Git-LFS pointer, not the payload"
        ;;
      "<!DOCTYPE"* | "<html"* | "<HTML"*)
        reason="HTML error page, not the payload"
        ;;
      "{"*'"message"'*)
        reason="JSON API error, not the payload"
        ;;
    esac
  fi

  if [ -n "${reason}" ]; then
    printf 'prune-corrupt-fixtures: removing %s (%s)\n' "${f}" "${reason}"
    rm -f -- "${f}"
    pruned=$((pruned + 1))
  fi
done < <(find "${root}" -type f -print0)

printf 'prune-corrupt-fixtures: scanned %d file(s), removed %d\n' "${scanned}" "${pruned}"

# Tracked files are the checkout's, never the cache's (see the header). Only
# with --restore-tracked: on a developer's checkout the same command would
# discard uncommitted edits. Outside a work tree there is nothing to restore.
if [ "${restore_tracked}" -eq 1 ] && git -C "${root}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  # No mapfile/readarray: macOS ships bash 3.2. The NUL-delimited read loop
  # also keeps odd file names intact.
  stale=()
  while IFS= read -r -d '' f; do
    stale+=("${f}")
  done < <(git -C "${root}" ls-files -z --deleted --modified -- . | sort -zu)
  for f in ${stale[@]+"${stale[@]}"}; do
    printf 'prune-corrupt-fixtures: restoring tracked file %s from the checkout\n' "${root}/${f}"
  done
  if [ "${#stale[@]}" -gt 0 ]; then
    git -C "${root}" checkout -- "${stale[@]}"
  fi
  printf 'prune-corrupt-fixtures: restored %d tracked file(s)\n' "${#stale[@]}"
fi
