#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Fetch FFmpeg's nv-codec-headers at the commit build-config.env pins.
#
# Usage:
#   fetch-nv-codec-headers.sh [--config FILE] DEST
#
# DEST must not exist yet. The script fetches NV_CODEC_HEADERS_COMMIT (never a
# branch) from each remote in turn, three attempts per remote, checks out that
# commit, compares `git rev-parse HEAD` with the pin and checks that the loader
# header libvmaf compiles in is there. Any other outcome exits non-zero; there is
# no unpinned fallback. Installing is the caller's step: `make -C DEST install` on
# Linux, a copy of DEST/include/ffnvcodec on Windows.
#
# NV_CODEC_HEADERS_REMOTES (space-separated) replaces the remote list and
# NV_CODEC_HEADERS_RETRY_DELAY (seconds, default 10) the pause between attempts;
# scripts/ci/tests/test_nv_codec_headers_single_source.py uses both.

set -euo pipefail
export LC_ALL=C

readonly ATTEMPTS=3
readonly DEFAULT_REMOTES="https://code.ffmpeg.org/FFmpeg/nv-codec-headers.git https://github.com/FFmpeg/nv-codec-headers.git"

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# A caller's GIT_DIR (a hook's, say) would turn the fetch into one of its repository.
# shellcheck source=scripts/lib/drop-git-env.sh
. "$root/scripts/lib/drop-git-env.sh"

config="$root/build-config.env"
if [ "${1-}" = "--config" ]; then
  config="${2:?--config needs a file}"
  shift 2
fi
dest="${1:?usage: fetch-nv-codec-headers.sh [--config FILE] DEST}"

if [ ! -f "$config" ]; then
  echo "fetch-nv-codec-headers: $config not found" >&2
  exit 2
fi
commit=$(sed -n 's/^NV_CODEC_HEADERS_COMMIT="\([0-9a-f]\{40\}\)"$/\1/p' "$config")
if [ -z "$commit" ]; then
  echo "fetch-nv-codec-headers: $config has no NV_CODEC_HEADERS_COMMIT of 40 hex digits" >&2
  exit 2
fi
if [ -e "$dest" ]; then
  echo "fetch-nv-codec-headers: $dest exists" >&2
  exit 2
fi

git init -q "$dest"
fetched=""
for remote in ${NV_CODEC_HEADERS_REMOTES:-$DEFAULT_REMOTES}; do
  for attempt in $(seq 1 "$ATTEMPTS"); do
    if git -C "$dest" fetch -q --depth=1 "$remote" "$commit"; then
      fetched="$remote"
      break 2
    fi
    echo "fetch-nv-codec-headers: attempt $attempt of $ATTEMPTS from $remote failed" >&2
    if [ "$attempt" -lt "$ATTEMPTS" ]; then
      sleep "${NV_CODEC_HEADERS_RETRY_DELAY:-10}"
    fi
  done
done
if [ -z "$fetched" ]; then
  echo "fetch-nv-codec-headers: no remote served commit $commit" >&2
  exit 1
fi

git -C "$dest" -c advice.detachedHead=false checkout -q FETCH_HEAD
head=$(git -C "$dest" rev-parse HEAD)
if [ "$head" != "$commit" ]; then
  echo "fetch-nv-codec-headers: $fetched gave $head, not $commit" >&2
  exit 1
fi
if [ ! -f "$dest/include/ffnvcodec/dynlink_loader.h" ]; then
  echo "fetch-nv-codec-headers: $commit has no include/ffnvcodec/dynlink_loader.h" >&2
  exit 1
fi
echo "nv-codec-headers $commit from $fetched"
