#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# scripts/upstream-watcher/check_ffmpeg_av1_videotoolbox.sh
#
# Polls FFmpeg upstream master for the appearance of an AV1
# VideoToolbox **encoder**. Reports YES (encoder landed) / NO (still
# absent) on stdout and exits 0 on YES, 1 on NO, 2 on infrastructure
# failure (network, missing tools).
#
# Background: see ADR-0339 (av1_videotoolbox placeholder + watcher).
# As of FFmpeg n8.1 / master 8518599cd1 (2026-05-09), only the AV1
# VideoToolbox **decoder** hwaccel exists
# (``ff_av1_videotoolbox_hwaccel``); the encoder side of
# ``libavcodec/videotoolboxenc.c`` registers H264 / HEVC / PRORES only.
# Apple's M3+ silicon has hardware AV1 encode capability but FFmpeg
# has not exposed it yet.
#
# Detection strategy: the encoder lands when ``videotoolboxenc.c``
# starts referencing ``AV_CODEC_ID_AV1`` (every encoder file matches
# its codec ID into the encoder struct). Grep for that sentinel in
# the upstream tree.
#
# Usage:
#   scripts/upstream-watcher/check_ffmpeg_av1_videotoolbox.sh
#       [--remote URL] [--ref REF] [--quiet]
#
# Exit codes:
#   0 — encoder is present in the polled tree
#   1 — encoder is NOT present (still upstream-blocked)
#   2 — infrastructure failure (network, git unavailable, etc.)

set -euo pipefail

REMOTE="https://git.ffmpeg.org/ffmpeg.git"
REF="refs/heads/master"
QUIET=0
ENCODER_FILE="libavcodec/videotoolboxenc.c"
SENTINEL="AV_CODEC_ID_AV1"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --remote)
      REMOTE="$2"
      shift 2
      ;;
    --ref)
      REF="$2"
      shift 2
      ;;
    --quiet)
      QUIET=1
      shift
      ;;
    -h | --help)
      sed -n 's/^# \{0,1\}//p' "$0" | sed -n '/^Usage:/,/^Exit codes:/p'
      exit 0
      ;;
    *)
      echo "unknown flag: $1" >&2
      exit 2
      ;;
  esac
done

log() {
  if [ "$QUIET" -eq 0 ]; then
    printf '%s\n' "$*" >&2
  fi
}

command -v git >/dev/null 2>&1 || {
  echo "git not found" >&2
  exit 2
}

log "[upstream-watcher] polling ${REMOTE} ${REF} for ${SENTINEL} in ${ENCODER_FILE}"

# 1. Resolve the tip SHA via ls-remote — cheap, no clone.
TIP="$(git ls-remote "$REMOTE" "$REF" 2>/dev/null | awk '{print $1}')"
if [ -z "${TIP:-}" ]; then
  echo "[upstream-watcher] FAILED to resolve ${REF} on ${REMOTE}" >&2
  exit 2
fi
log "[upstream-watcher] tip: ${TIP}"

# 2. Shallow-fetch ONLY the encoder file's blob via a partial clone.
#    Cheaper than a full clone but still self-contained — works on
#    any git >= 2.25 and needs no special server config beyond
#    standard smart-HTTP.
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if ! git -C "$WORK" init --quiet 2>/dev/null; then
  echo "[upstream-watcher] git init failed" >&2
  exit 2
fi

git -C "$WORK" remote add origin "$REMOTE"
if ! git -C "$WORK" fetch --depth 1 --filter=blob:none --quiet origin "$TIP" 2>/dev/null; then
  echo "[upstream-watcher] FAILED to fetch ${TIP} from ${REMOTE}" >&2
  exit 2
fi

# Pull just the one path we care about.
if ! git -C "$WORK" sparse-checkout init --cone >/dev/null 2>&1; then
  echo "[upstream-watcher] sparse-checkout init failed" >&2
  exit 2
fi
git -C "$WORK" sparse-checkout set "libavcodec" >/dev/null

if ! git -C "$WORK" checkout --quiet FETCH_HEAD 2>/dev/null; then
  echo "[upstream-watcher] checkout failed" >&2
  exit 2
fi

ABS_FILE="${WORK}/${ENCODER_FILE}"
if [ ! -f "$ABS_FILE" ]; then
  echo "[upstream-watcher] expected file ${ENCODER_FILE} missing from upstream tree" >&2
  exit 2
fi

# 3. Grep for the sentinel.
if grep -q "$SENTINEL" "$ABS_FILE"; then
  cat <<EOF
status: YES
encoder: av1_videotoolbox
remote: ${REMOTE}
ref: ${REF}
tip_sha: ${TIP}
file: ${ENCODER_FILE}
sentinel: ${SENTINEL}
note: AV1 VideoToolbox encoder has landed in FFmpeg upstream. Activate the placeholder adapter (tools/vmaf-tune/src/vmaftune/codec_adapters/av1_videotoolbox.py).
EOF
  exit 0
else
  cat <<EOF
status: NO
encoder: av1_videotoolbox
remote: ${REMOTE}
ref: ${REF}
tip_sha: ${TIP}
file: ${ENCODER_FILE}
sentinel: ${SENTINEL}
note: AV1 VideoToolbox encoder NOT yet present upstream. Placeholder adapter remains inactive.
EOF
  exit 1
fi
