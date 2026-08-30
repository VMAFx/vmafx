#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# check-compose-dri-writable.sh — lint gate for dev/docker-compose.yml.
#
# Regression guard for the bug fixed in PR #707: the smoke-probe-cron service
# had `read_only: true` on its /dev/dri bind-mount.  The NVIDIA Container
# Toolkit OCI hook writes PCI-bus-address symlinks into /dev/dri/by-path at
# container init; a read-only bind-mount causes the hook to fail with
# "read-only file system" on every NVIDIA host, preventing the container
# from starting at all (ADR-0529).
#
# This script asserts that every /dev/dri bind-mount in docker-compose.yml
# is NOT marked read_only: true.  It uses a state machine over the YAML
# lines rather than a YAML parser to stay dependency-free.
#
# Exit codes:
#   0  — all /dev/dri mounts are writable (or no /dev/dri mounts found)
#   1  — at least one /dev/dri mount has read_only: true
#   2  — compose file not found

set -euo pipefail

COMPOSE_FILE="${1:-$(git rev-parse --show-toplevel)/dev/docker-compose.yml}"

if [[ ! -f "$COMPOSE_FILE" ]]; then
  echo "ERROR: compose file not found: $COMPOSE_FILE" >&2
  exit 2
fi

fail=0
in_dri_block=0
line_no=0
prev_target_line=0

while IFS= read -r line; do
  line_no=$((line_no + 1))

  # Detect `target: /dev/dri` — enter the DRI block.
  if [[ "$line" =~ target:[[:space:]]*/dev/dri ]]; then
    in_dri_block=1
    prev_target_line=$line_no
    continue
  fi

  # When inside a DRI block, look for read_only: true.
  if [[ $in_dri_block -eq 1 ]]; then
    # A new volume entry (leading "- ") or a new key at the same or
    # higher indent level ends the DRI block.
    if [[ "$line" =~ ^[[:space:]]*-[[:space:]] ]] ||
      [[ "$line" =~ ^[[:space:]]*(type|source|target|read_only|volumes|services|networks):[[:space:]] &&
        ! "$line" =~ target:[[:space:]]*/dev/dri ]]; then
      # If we see target again, we're in a new mount block already handled above.
      # Only reset if not a mount-level key.
      if [[ "$line" =~ ^[[:space:]]*(type|source):[[:space:]] ]]; then
        # still in same mount block — continue scanning
        :
      elif [[ "$line" =~ ^[[:space:]]*read_only:[[:space:]] ]]; then
        : # handled below
      else
        in_dri_block=0
      fi
    fi

    if [[ "$line" =~ read_only:[[:space:]]*true ]]; then
      echo "FAIL: /dev/dri bind-mount at line ${prev_target_line} has read_only: true" \
        "(file: $COMPOSE_FILE, offending line: ${line_no})" >&2
      echo "      The NVIDIA Container Toolkit OCI hook writes to /dev/dri/by-path" \
        "at container init; read_only:true causes it to fail (PR #707, ADR-0529)." >&2
      fail=1
      in_dri_block=0
    fi

    if [[ "$line" =~ read_only:[[:space:]]*false ]]; then
      in_dri_block=0
    fi
  fi
done <"$COMPOSE_FILE"

if [[ $fail -eq 0 ]]; then
  echo "OK: all /dev/dri bind-mounts in $COMPOSE_FILE are writable."
fi

exit $fail
