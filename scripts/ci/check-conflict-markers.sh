#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# check-conflict-markers.sh — belt-and-suspenders conflict-marker guard.
#
# Companion to the `check-merge-conflict` pre-commit-hooks entry (Python).
# This shell hook uses `git grep` (git's own C binary) for near-zero overhead
# (< 5ms on a typical staged tree). Both hooks fire at pre-commit time;
# if either triggers, the merge was not fully resolved.
#
# PR #50 shipped unresolved conflict markers into 70 files across docs,
# YAML, CI workflows, and a CUDA source file. This hook would have blocked it.
#
# Exit 0 = no markers found (clean commit).
# Exit 1 = at least one marker found (commit blocked, offending lines printed).

set -euo pipefail

# Collect the names of staged (cached) text files.
staged=$(git diff --cached --name-only --diff-filter=ACM 2>/dev/null)

if [ -z "$staged" ]; then
  exit 0
fi

# git grep -I skips binary files. -l lists only filenames; -n adds line numbers.
# We search for all three conflict marker patterns simultaneously. The patterns
# are anchored at the start of a line (^) to avoid false positives in prose.
# shellcheck disable=SC2086  # $staged is intentionally word-split (one file per word)
if git grep -I -n -E "^(<{7} |={7}$|>{7} )" -- $staged 2>/dev/null; then
  echo ""
  echo "ERROR: git conflict markers found in staged files."
  echo "Resolve all merge conflicts before committing."
  exit 1
fi

exit 0
