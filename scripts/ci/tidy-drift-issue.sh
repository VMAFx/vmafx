#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# tidy-drift-issue.sh - open or update the one issue that reports clang-tidy
# ratchet drift found by a scheduled run (ADR-2796, Q-315).
#
# Usage: tidy-drift-issue.sh OWNER/REPO RUN_ID
#
# Reads the failed jobs of the workflow run through the API. With none failed
# it exits 1 and writes nothing: an issue that names no lane would be noise,
# and a caller that reaches this script without a failure has a wiring bug.
# With failures it finds the open issue titled exactly "$TITLE" and appends a
# comment, or creates the issue when there is none, so a persistent drift is
# one issue with a trail of runs, not one issue per night.
#
# GH_TOKEN must allow issues: write and actions: read. TIDY_DRIFT_GH names the
# gh binary (tests substitute a stand-in).
set -euo pipefail
export LC_ALL=C

readonly TITLE="Tidy ratchet drift on master"
readonly GH="${TIDY_DRIFT_GH:-gh}"
readonly API_TIMEOUT=120

die() {
  printf 'tidy-drift-issue: %s\n' "$1" >&2
  exit "${2:-1}"
}

[ "$#" -eq 2 ] || die "usage: tidy-drift-issue.sh OWNER/REPO RUN_ID" 2
repo="$1"
run_id="$2"
case "$repo" in */*) ;; *) die "repository must be OWNER/REPO, got '$repo'" 2 ;; esac
case "$run_id" in '' | *[!0-9]*) die "run id must be a number, got '$run_id'" 2 ;; esac

failed="$(timeout "$API_TIMEOUT" "$GH" api "repos/$repo/actions/runs/$run_id/jobs?per_page=100" \
  --jq '[.jobs[] | select(.conclusion == "failure") | .name] | join("\n")')" ||
  die "cannot read the jobs of run $run_id"
[ -n "$failed" ] || die "run $run_id has no failed job; nothing to report"

run_url="https://github.com/$repo/actions/runs/$run_id"
body="$(
  printf 'The scheduled clang-tidy sweep of master found drift: %s\n\n' "$run_url"
  printf 'Failed lanes:\n\n'
  printf '%s\n' "$failed" | sed 's/^/- /'
  printf '\nEach lane fails closed against scripts/ci/tidy-baseline-<lane>.json. Fix the code'
  printf ' (never the baseline by hand), then re-measure with scripts/dev/tidy-lane.sh.\n'
  printf 'See docs/development/tidy-ratchet.md, "Hosted lanes".\n'
)"

existing="$(timeout "$API_TIMEOUT" "$GH" issue list --repo "$repo" --state open \
  --search "\"$TITLE\" in:title" --json number,title \
  --jq "[.[] | select(.title == \"$TITLE\") | .number] | first // empty")" ||
  die "cannot list the open issues"

if [ -n "$existing" ]; then
  timeout "$API_TIMEOUT" "$GH" issue comment "$existing" --repo "$repo" --body "$body" ||
    die "cannot comment on issue #$existing"
  printf 'tidy-drift-issue: updated #%s\n' "$existing"
else
  timeout "$API_TIMEOUT" "$GH" issue create --repo "$repo" --title "$TITLE" --body "$body" ||
    die "cannot create the issue"
  printf 'tidy-drift-issue: opened a new issue\n'
fi
