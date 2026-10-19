#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Query GitHub API count endpoints with exponential backoff and fail-closed
# integer verification (ADR-0448). A rate-limit reply that states its delay
# ("try again in 84.5s") is waited out: the retry sleeps the stated delay
# rounded up plus one second when that is longer than the backoff step. Three
# attempts (two retries); a stated delay above UPSTREAM_WATCH_MAX_WAIT_S (default
# 120) per retry, or a total above UPSTREAM_WATCH_TOTAL_WAIT_S (default 300),
# fails at once, so a run that exhausts the bound stays red.
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
sleeps=(15 30)
max_wait="${UPSTREAM_WATCH_MAX_WAIT_S:-120}"
total_budget="${UPSTREAM_WATCH_TOTAL_WAIT_S:-300}"
waited=0
last_output=""

tmp_err=$(mktemp)
trap 'rm -f "$tmp_err"' EXIT

for attempt in 1 2 3; do
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

  if [ "$attempt" -lt 3 ]; then
    sleep_idx=$((attempt - 1))
    sleep_base="${sleeps[$sleep_idx]}"
    stated=""
    if [[ "$last_output" =~ try\ again\ in\ ([0-9]+)(\.[0-9]+)?s ]]; then
      stated=$((BASH_REMATCH[1] + 1))
      if [ -n "${BASH_REMATCH[2]}" ]; then
        stated=$((stated + 1))
      fi
    fi
    if [ -n "$stated" ]; then
      if [ "$stated" -gt "$max_wait" ]; then
        echo "::error::stated retry delay ${stated}s exceeds the ${max_wait}s bound" >&2
        break
      fi
      if [ "$stated" -gt "$sleep_base" ]; then
        sleep_base="$stated"
      fi
    fi
    if [ $((waited + sleep_base)) -gt "$total_budget" ]; then
      echo "::error::retry wait would pass the ${total_budget}s total bound" >&2
      break
    fi
    waited=$((waited + sleep_base))
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
