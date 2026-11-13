#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# scripts/ci/download-release.sh — fetch one GitHub release asset through the
# repository's download retry policy (compat/python-vmaf/tools/retrying_download.py),
# so a short 5xx from github.com does not fail a required job. #3349.
#
# Usage: scripts/ci/download-release.sh URL DESTINATION
#
# URL must be an https://github.com/<owner>/<repo>/releases/download/... asset.
# DESTINATION is written through a sibling .part file; an existing file is replaced
# only after a complete download. Exit 0 on success, 1 after the policy's final
# failed attempt, 2 on usage.

set -euo pipefail

if [ "$#" -ne 2 ]; then
  echo "usage: download-release.sh URL DESTINATION" >&2
  exit 2
fi

url=$1
destination=$2
case "$url" in
  https://github.com/*/releases/download/*) ;;
  *)
    echo "download-release: not a GitHub release asset: $url" >&2
    exit 2
    ;;
esac

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
python=${PYTHON:-}
if [ -z "$python" ]; then
  if command -v python3 >/dev/null 2>&1; then
    python=python3
  elif command -v python >/dev/null 2>&1; then
    python=python
  else
    echo "download-release: python3 or python is required" >&2
    exit 2
  fi
fi

exec "$python" -I "$root/compat/python-vmaf/tools/retrying_download.py" "$url" "$destination"
