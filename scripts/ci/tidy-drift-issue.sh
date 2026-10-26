#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# tidy-drift-issue.sh - open or update the one issue that reports clang-tidy
# ratchet drift found by a scheduled run (ADR-2796, Q-315).
#
# Usage: tidy-drift-issue.sh OWNER/REPO RUN_ID
#
# Thin wrapper around scripts/ci/scheduled-failure-issue.sh.
#
# GH_TOKEN must allow issues: write and actions: read. TIDY_DRIFT_GH names the
# gh binary (tests substitute a stand-in).
set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR
readonly TITLE="Tidy ratchet drift on master"

die() {
  printf 'tidy-drift-issue: %s\n' "$1" >&2
  exit "${2:-1}"
}

[ "$#" -eq 2 ] || die "usage: tidy-drift-issue.sh OWNER/REPO RUN_ID" 2
repo="$1"
run_id="$2"
case "$repo" in */*) ;; *) die "repository must be OWNER/REPO, got '$repo'" 2 ;; esac
case "$run_id" in '' | *[!0-9]*) die "run id must be a number, got '$run_id'" 2 ;; esac

hint="$(
  cat <<'EOF'
Each lane fails closed against scripts/ci/tidy-baseline-<lane>.json. Fix the code (never the baseline by hand), then re-measure with scripts/dev/tidy-lane.sh.
See docs/development/tidy-ratchet.md, "Hosted lanes".
EOF
)"

exec "$SCRIPT_DIR/scheduled-failure-issue.sh" "$repo" "$run_id" "$TITLE" "$hint"
