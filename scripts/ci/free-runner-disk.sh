#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# scripts/ci/free-runner-disk.sh -- remove the preinstalled SDKs a hosted Ubuntu runner carries
# and no job of this repository uses, so a large image build has room on the root disk. On
# ubuntu-26.04 the dev container build ended with `No space left on device` without it
# (docs/state.md row T-CI-DEV-CONTAINER-DISK-UBUNTU-2604-2026-10-11).
#
# Usage:
#   free-runner-disk.sh           remove the directories (Linux runner, sudo) and print the
#                                 free space of / before and after
#   free-runner-disk.sh --list    print the directories, one per line (no root;
#                                 scripts/ci/tests/test_free_runner_disk.py)
#
# Exit 0 removed or listed, 1 a removal failed, 2 usage.

set -euo pipefail

DIRS=(
  /usr/share/dotnet
  /usr/local/lib/android
  /opt/ghc
  /opt/hostedtoolcache/CodeQL
)

case "${1:-}" in
  --list)
    if [[ $# -ne 1 ]]; then
      echo "usage: $0 [--list]" >&2
      exit 2
    fi
    printf '%s\n' "${DIRS[@]}"
    exit 0
    ;;
  "")
    ;;
  *)
    echo "usage: $0 [--list]" >&2
    exit 2
    ;;
esac

df -h / | tail -n 1
sudo rm -rf -- "${DIRS[@]}"
df -h / | tail -n 1
