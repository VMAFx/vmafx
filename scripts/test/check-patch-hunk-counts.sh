#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# check-patch-hunk-counts.sh — verify every hunk header in every
# ffmpeg-patches/00*-*.patch declares the correct line counts.
#
# PR #851 context: PR #850 expanded do_vmaf_sycl in 0005 from ~36 lines
# to ~77 lines but did not update the @@ -N,M +N,M @@ header to match.
# git apply / git am silently truncated the hunk at the declared 280-line
# boundary; the resulting vf_libvmaf.c was 44 lines short, causing 0006
# to fail with a "context not found" error.
#
# This script re-counts the actual new-side lines in every hunk (context
# lines + added lines, i.e. every line that is NOT a deletion) and compares
# against the declared +count in the @@ header.  A mismatch means a hunk
# header is stale — exactly the failure mode PR #851 fixed.
#
# Usage:
#   scripts/test/check-patch-hunk-counts.sh [patch-glob]
#
# Exit codes:
#   0  all hunks match
#   1  at least one mismatch (printed to stderr)
#
# Limitation: this script validates hunk header arithmetic only.  It does
# NOT attempt to apply the patch, so context-line mismatches (wrong line
# numbers) are not detected here — use `git apply --check` in a clean tree
# for that.

set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || echo ".")"
PATCH_DIR="${REPO_ROOT}/ffmpeg-patches"
GLOB="${1:-"${PATCH_DIR}/00*-*.patch"}"

fail=0

for patchfile in ${GLOB}; do
  [ -f "${patchfile}" ] || continue
  pname="$(basename "${patchfile}")"
  hunk=0
  declared_add=0
  actual_add=0
  in_hunk=0

  while IFS= read -r line; do
    case "${line}" in
      @@\ *)
        # Flush previous hunk
        if [ "${in_hunk}" -eq 1 ]; then
          if [ "${actual_add}" -ne "${declared_add}" ]; then
            printf 'FAIL %s hunk %d: declared +%d lines, actual +%d lines\n' \
              "${pname}" "${hunk}" "${declared_add}" "${actual_add}" >&2
            fail=1
          fi
        fi
        hunk=$((hunk + 1))
        in_hunk=1
        actual_add=0
        # Extract +<start>,<count> or +<start> (count=1 when omitted).
        # The declared count is the total new-side line count:
        #   context lines + added lines (NOT deletion lines).
        count_part="${line#*+}"
        count_part="${count_part%% *}"
        if [[ "${count_part}" == *,* ]]; then
          declared_add="${count_part#*,}"
        else
          declared_add=1
        fi
        ;;
      "-- ")
        # git patch trailer — end of last hunk
        in_hunk=0
        ;;
      "\ No newline"*)
        # "\ No newline at end of file" marker — not a content line, skip
        ;;
      ---* | +++*)
        # diff file-header lines — not part of hunk body, skip
        ;;
      -*)
        # deletion line — counts only toward old-side, skip for new-side count
        ;;
      +*)
        # addition line — counts toward new-side
        if [ "${in_hunk}" -eq 1 ]; then
          actual_add=$((actual_add + 1))
        fi
        ;;
      " "* | "")
        # context line (starts with a space, or bare empty line) — new-side
        if [ "${in_hunk}" -eq 1 ]; then
          actual_add=$((actual_add + 1))
        fi
        ;;
    esac
  done <"${patchfile}"

  # Flush last hunk
  if [ "${in_hunk}" -eq 1 ]; then
    if [ "${actual_add}" -ne "${declared_add}" ]; then
      printf 'FAIL %s hunk %d: declared +%d lines, actual +%d lines\n' \
        "${pname}" "${hunk}" "${declared_add}" "${actual_add}" >&2
      fail=1
    fi
  fi
done

if [ "${fail}" -eq 0 ]; then
  echo "check-patch-hunk-counts: all hunks OK"
fi
exit "${fail}"
