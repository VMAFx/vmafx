#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# ensure_label.sh - ensure a GitHub label exists in the repository (idempotent).
#
# Usage: ensure_label.sh OWNER/REPO NAME COLOR DESCRIPTION
#
# Runs gh label create with --force so an existing label is updated rather than
# failing, then reads the label list until the label shows up: a label created
# moments before can still be unknown to `gh issue create --label`, which failed
# the 2026-10-27 scheduled run (T-UPSTREAM-WATCHER-LABEL-RACE-2026-10-27). The
# read is bounded (LABEL_WAIT_ATTEMPTS, UPSTREAM_WATCHER_LABEL_WAIT seconds
# apart); a label that never shows up fails naming it.
#
# GH_TOKEN must allow issues: write. UPSTREAM_WATCHER_GH names the gh binary
# (tests substitute a stand-in).
set -euo pipefail
export LC_ALL=C

readonly GH="${UPSTREAM_WATCHER_GH:-gh}"
readonly API_TIMEOUT=120
readonly LABEL_WAIT_ATTEMPTS=10
readonly LABEL_WAIT="${UPSTREAM_WATCHER_LABEL_WAIT:-3}"

die() {
  printf 'ensure_label: %s\n' "$1" >&2
  exit "${2:-1}"
}

[ "$#" -eq 4 ] || die "usage: ensure_label.sh OWNER/REPO NAME COLOR DESCRIPTION" 2

repo="$1"
name="$2"
color="$3"
description="$4"

case "$repo" in */*) ;; *) die "repository must be OWNER/REPO, got '$repo'" 2 ;; esac
[ -n "$name" ] || die "label name cannot be empty" 2
[ -n "$color" ] || die "color cannot be empty" 2
[ -n "$description" ] || die "description cannot be empty" 2

if ! timeout "$API_TIMEOUT" "$GH" label create "$name" --repo "$repo" --color "$color" --description "$description" --force; then
  printf '::error::label %s could not be created in %s\n' "$name" "$repo" >&2
  exit 1
fi

label_listed() {
  local found
  found="$(timeout "$API_TIMEOUT" "$GH" label list --repo "$repo" --search "$name" \
    --limit 100 --json name --jq ".[] | select(.name == \"$name\") | .name")" || return 1
  [ "$found" = "$name" ]
}

attempt=1
until label_listed; do
  if [ "$attempt" -ge "$LABEL_WAIT_ATTEMPTS" ]; then
    printf '::error::label %s was created in %s but is not listed after %s reads\n' \
      "$name" "$repo" "$LABEL_WAIT_ATTEMPTS" >&2
    exit 1
  fi
  attempt=$((attempt + 1))
  sleep "$LABEL_WAIT"
done
