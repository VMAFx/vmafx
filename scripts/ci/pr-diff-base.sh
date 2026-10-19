# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
# shellcheck shell=bash

# scripts/ci/pr-diff-base.sh — the commit a pull request's diff starts from.
# Sourced, never executed, by the pull request gates that read the diff from
# BASE_SHA / HEAD_SHA: deliverables-check.sh, state-md-touch-check.sh,
# ffmpeg-patches-surface-check.sh, classify-dependency-pr.sh and
# release-pr-exempt.sh.
#
# GitHub's `pull_request.base.sha` is the base branch tip when the event fired,
# not the commit the branch forked from. Once master moves, a two-dot diff from
# it reports every file master changed since the fork as the pull request's own
# change, among them the files rendered when pull requests land (CHANGELOG.md,
# the ADR index), so a pull request that fell behind failed the ADR-2197
# rendered-file check without touching one. The diff starts at the merge base
# of the two commits instead, which is what `base...head` means.
#
# When no merge base can be computed (a shallow checkout, a commit the clone
# does not have, unrelated histories) the caller fails closed. It never diffs
# from the base tip instead: that is the defect this file exists to remove.

# pr_diff_base BASE HEAD CALLER — print the merge base of BASE and HEAD. When
# there is none, print why to stderr, prefixed with CALLER, and return 1.
pr_diff_base() {
  local base=$1 head=$2 caller=$3 merge_base=""
  if merge_base="$(git merge-base "$base" "$head" 2>/dev/null)" && [ -n "$merge_base" ]; then
    printf '%s\n' "$merge_base"
    return 0
  fi
  printf '%s: no merge base of %s and %s; the checkout needs both commits and the history between them (actions/checkout fetch-depth: 0). The diff is not taken from the base tip instead.\n' \
    "$caller" "$base" "$head" >&2
  return 1
}
