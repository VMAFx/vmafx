#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# ensure_label.sh - ensure a GitHub label exists in the repository (idempotent).
#
# Usage: ensure_label.sh OWNER/REPO NAME COLOR DESCRIPTION
#
# Runs gh label create with --force so an existing label is updated rather than
# failing.
#
# GH_TOKEN must allow issues: write. UPSTREAM_WATCHER_GH names the gh binary
# (tests substitute a stand-in).
set -euo pipefail
export LC_ALL=C

readonly GH="${UPSTREAM_WATCHER_GH:-gh}"
readonly API_TIMEOUT=120

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
