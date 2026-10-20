#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# scripts/ci/go-fetch-retry.sh -- run a Go command that downloads modules, retrying when the
# module proxy drops the connection. Master run 39313529937 failed `go install` of gosec on
# "stream error: stream ID 123; INTERNAL_ERROR; received from peer" from proxy.golang.org: a
# transient fault of the proxy, not of the code. A retry downloads the same pinned versions
# (go.sum and the @version still verify every module), so it moves no result.
#
# Usage:
#   go-fetch-retry.sh ARGS...      run `go ARGS...`, at most GO_FETCH_ATTEMPTS (1-10, default 3) times
#
# GO_BIN overrides the go binary (scripts/ci/tests/test_go_fetch_retry.py). The pause after
# attempt n is n * GO_FETCH_DELAY seconds (default 10). Exit 0 when an attempt succeeds, the
# last attempt's status when all fail, 2 usage.

set -euo pipefail

if [ "$#" -eq 0 ]; then
  echo "usage: go-fetch-retry.sh ARGS..." >&2
  exit 2
fi

go_bin="${GO_BIN:-go}"
attempts="${GO_FETCH_ATTEMPTS:-3}"
delay="${GO_FETCH_DELAY:-10}"
case "${attempts}" in
  '' | *[!0-9]* | 0 | 0*)
    echo "go-fetch-retry: GO_FETCH_ATTEMPTS must be a positive integer, got '${attempts}'" >&2
    exit 2
    ;;
esac
if [ "${attempts}" -gt 10 ]; then
  echo "go-fetch-retry: GO_FETCH_ATTEMPTS is at most 10, got '${attempts}'" >&2
  exit 2
fi
case "${delay}" in
  '' | *[!0-9]*)
    echo "go-fetch-retry: GO_FETCH_DELAY must be a non-negative integer, got '${delay}'" >&2
    exit 2
    ;;
esac

status=0
for attempt in $(seq 1 "${attempts}"); do
  status=0
  "${go_bin}" "$@" || status=$?
  if [ "${status}" -eq 0 ]; then
    exit 0
  fi
  if [ "${attempt}" -lt "${attempts}" ]; then
    echo "go-fetch-retry: attempt ${attempt} of ${attempts} failed (status ${status}); retrying in $((attempt * delay))s" >&2
    sleep "$((attempt * delay))"
  fi
done
echo "go-fetch-retry: 'go $*' failed ${attempts} times, last status ${status}" >&2
exit "${status}"
