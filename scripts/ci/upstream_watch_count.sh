#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Query GitHub API count endpoints with exponential backoff and fail-closed
# integer verification (ADR-0448).
#
# Usage:
#   upstream_watch_count.sh <gh api args...>

set -euo pipefail

if [ "$#" -eq 0 ]; then
  echo "::error::upstream_watch_count.sh: missing gh api arguments" >&2
  exit 1
fi

gh_args=("$@")
if [ "${1:-}" = "api" ]; then
  gh_args=("${@:2}")
fi

scale="${UPSTREAM_WATCH_SLEEP_SCALE:-1}"
sleeps=(15 30 60)
last_output=""

tmp_err=$(mktemp)
trap 'rm -f "$tmp_err"' EXIT

for attempt in 1 2 3 4; do
  : >"$tmp_err"
  exit_code=0
  out=$(gh api "${gh_args[@]}" 2>"$tmp_err") || exit_code=$?
  err=$(cat "$tmp_err")

  trimmed="${out#"${out%%[![:space:]]*}"}"
  trimmed="${trimmed%"${trimmed##*[![:space:]]}"}"

  if [ "$exit_code" -eq 0 ] && [[ "$trimmed" =~ ^[0-9]+$ ]]; then
    printf '%s\n' "$trimmed"
    exit 0
  fi

  if [ -n "$err" ] && [ -n "$out" ]; then
    last_output="${out}
${err}"
  elif [ -n "$err" ]; then
    last_output="$err"
  elif [ -n "$out" ]; then
    last_output="$out"
  else
    last_output="gh api exited with status ${exit_code}"
  fi

  if [ "$attempt" -lt 4 ]; then
    sleep_idx=$((attempt - 1))
    sleep_base="${sleeps[$sleep_idx]}"
    if [ "$scale" = "0" ]; then
      sleep_s=0
    elif [[ "$scale" =~ ^[0-9]+$ ]]; then
      sleep_s=$((sleep_base * scale))
    else
      sleep_s=$(awk -v b="$sleep_base" -v s="$scale" 'BEGIN { print b * s }')
    fi
    if [ "$sleep_s" != "0" ] && [ "$sleep_s" != "0.0" ]; then
      sleep "$sleep_s"
    fi
  fi
done

echo "::error::${last_output}" >&2
exit 1
