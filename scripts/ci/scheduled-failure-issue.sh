#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# scheduled-failure-issue.sh - open or update a tracking issue for a scheduled
# workflow run failure (ADR-2796, Q-315).
#
# Usage: scheduled-failure-issue.sh OWNER/REPO RUN_ID TITLE HINT
#
# Reads the failed jobs of the workflow run through the API. The jobs API can
# lag the run's own `needs` results (run 40515965410 read no failure 46 s after
# the failed job completed), so an empty read is repeated, at most
# FAILED_JOB_READS times SCHEDULED_FAILURE_WAIT seconds apart. With still none
# failed it prints a notice and exits 0 without writing: an issue that names no
# job would be noise, and failing the reporter would turn the run red twice.
# With failures it finds the open issue titled exactly "$TITLE" and appends a
# comment, or creates the issue when there is none, so a persistent failure is
# one issue with a trail of runs, not one issue per scheduled run.
#
# GH_TOKEN must allow issues: write and actions: read.
# SCHEDULED_FAILURE_GH names the gh binary (falling back to TIDY_DRIFT_GH then gh).
set -euo pipefail
export LC_ALL=C

readonly GH="${SCHEDULED_FAILURE_GH:-${TIDY_DRIFT_GH:-gh}}"
readonly API_TIMEOUT=120
readonly FAILED_JOB_READS=6
readonly READ_WAIT="${SCHEDULED_FAILURE_WAIT:-10}"

die() {
  printf 'scheduled-failure-issue: %s\n' "$1" >&2
  exit "${2:-1}"
}

[ "$#" -eq 4 ] || die "usage: scheduled-failure-issue.sh OWNER/REPO RUN_ID TITLE HINT" 2
repo="$1"
run_id="$2"
title="$3"
hint="$4"

case "$repo" in */*) ;; *) die "repository must be OWNER/REPO, got '$repo'" 2 ;; esac
case "$run_id" in '' | *[!0-9]*) die "run id must be a number, got '$run_id'" 2 ;; esac
[ -n "$title" ] || die "title cannot be empty" 2
[ -n "$hint" ] || die "hint cannot be empty" 2

read_failed() {
  timeout "$API_TIMEOUT" "$GH" api "repos/$repo/actions/runs/$run_id/jobs?per_page=100" \
    --jq '[.jobs[] | select(.conclusion == "failure") | .name] | join("\n")' ||
    die "cannot read the jobs of run $run_id"
}

failed="$(read_failed)"
attempt=1
while [ -z "$failed" ] && [ "$attempt" -lt "$FAILED_JOB_READS" ]; do
  sleep "$READ_WAIT"
  attempt=$((attempt + 1))
  failed="$(read_failed)"
done
if [ -z "$failed" ]; then
  printf '::notice::run %s has no failed job after %s reads; nothing to report\n' \
    "$run_id" "$FAILED_JOB_READS"
  exit 0
fi

run_url="https://github.com/$repo/actions/runs/$run_id"
body="$(
  printf '%s: %s\n\n' "$title" "$run_url"
  printf 'Failed jobs:\n\n'
  printf '%s\n' "$failed" | sed 's/^/- /'
  printf '\n\n%s\n' "$hint"
)"

existing="$(timeout "$API_TIMEOUT" "$GH" issue list --repo "$repo" --state open \
  --search "\"$title\" in:title" --json number,title \
  --jq "[.[] | select(.title == \"$title\") | .number] | first // empty")" ||
  die "cannot list the open issues"

if [ -n "$existing" ]; then
  timeout "$API_TIMEOUT" "$GH" issue comment "$existing" --repo "$repo" --body "$body" ||
    die "cannot comment on issue #$existing"
  printf 'scheduled-failure-issue: updated #%s\n' "$existing"
else
  timeout "$API_TIMEOUT" "$GH" issue create --repo "$repo" --title "$title" --body "$body" ||
    die "cannot create the issue"
  printf 'scheduled-failure-issue: opened a new issue\n'
fi
