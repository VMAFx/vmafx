#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# Step 02: Generate the Phase A canonical-6 corpus from operator-supplied
# Netflix reference YUVs. Wraps scripts/dev/hw_encoder_corpus.py in a
# loop over (source x encoder x cqs), then concatenates per-source shards
# into the canonical JSONL the LOSO trainer consumes.
#
# Required environment:
#   REF_DIR        Directory of *.yuv reference clips (one file per source)
# Optional environment:
#   ENCODERS       Comma-separated encoder list (default: h264_nvenc)
#   CQS            Comma-separated CQ values (default: 19,25,31,37)
#   OUT_DIR        Run output dir (default: $REPO_ROOT/runs/ensemble_v2_real)
#   PHASE_A_DIR    Phase A output dir (default: $REPO_ROOT/runs/phase_a/full_grid)
#   LIBVMAF_BIN    libvmaf-CUDA binary (default: $REPO_ROOT/core/build-cuda/tools/vmaf)
#   WIDTH HEIGHT   Frame dimensions (default: 1920 1080)
#   PIX_FMT        Pixel format (default: yuv420p)
#   FRAMERATE      Source framerate (default: 25)

set -euo pipefail

KIT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "$KIT_DIR/../.." && pwd)}"

# shellcheck source=tools/ensemble-training-kit/_platform_detect.sh
# shellcheck disable=SC1091
source "$KIT_DIR/_platform_detect.sh"
PLATFORM="$(detect_platform)"

REF_DIR="${REF_DIR:?must set REF_DIR to the directory containing reference YUVs}"
# When ENCODERS is unset, default per-platform: NVIDIA -> *_nvenc, Intel
# iHD -> *_qsv, macOS -> *_videotoolbox, otherwise libx264.
ENCODERS="${ENCODERS:-$(detect_default_encoders)}"
CQS="${CQS:-19,25,31,37}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/runs/ensemble_v2_real}"
PHASE_A_DIR="${PHASE_A_DIR:-$REPO_ROOT/runs/phase_a/full_grid}"
LIBVMAF_BIN="${LIBVMAF_BIN:-$REPO_ROOT/core/build-cuda/tools/vmaf}"
WIDTH="${WIDTH:-1920}"
HEIGHT="${HEIGHT:-1080}"
PIX_FMT="${PIX_FMT:-yuv420p}"
FRAMERATE="${FRAMERATE:-25}"

if [[ ! -d "$REF_DIR" ]]; then
  echo "[corpus] error: REF_DIR '$REF_DIR' is not a directory" >&2
  exit 2
fi
mapfile -t YUVS < <(find "$REF_DIR" -maxdepth 1 -type f -name '*.yuv' | sort)
if [[ "${#YUVS[@]}" -eq 0 ]]; then
  echo "[corpus] error: no *.yuv files in $REF_DIR" >&2
  exit 2
fi

mkdir -p "$PHASE_A_DIR/per_source" "$OUT_DIR/logs"

IFS=',' read -ra ENCODER_LIST <<<"$ENCODERS"
IFS=',' read -ra CQ_LIST <<<"$CQS"

echo "[corpus] platform=$PLATFORM sources=${#YUVS[@]} encoders=${ENCODERS} cqs=${CQS}"
echo "[corpus] phase_a=$PHASE_A_DIR libvmaf=$LIBVMAF_BIN"
echo

# Optional QSV gate: skip QSV encoders if iHD missing.
have_ihd=0
if command -v vainfo >/dev/null 2>&1 && vainfo --display drm 2>/dev/null | grep -q iHD; then
  have_ihd=1
fi
# VideoToolbox gate: only available on Darwin with VT-linked ffmpeg.
have_vt=0
if [[ "$PLATFORM" == darwin-* ]] &&
  command -v ffmpeg >/dev/null 2>&1 &&
  ffmpeg -hide_banner -encoders 2>/dev/null | grep -q h264_videotoolbox; then
  have_vt=1
fi

t_total=$(date -u +%s)
for yuv in "${YUVS[@]}"; do
  src_stem="$(basename "$yuv" .yuv)"
  for enc in "${ENCODER_LIST[@]}"; do
    if [[ "$enc" == *_qsv ]] && [[ "$have_ihd" -eq 0 ]]; then
      echo "[corpus] skip $src_stem $enc (no iHD/QSV runtime)"
      continue
    fi
    if [[ "$enc" == *_videotoolbox ]] && [[ "$have_vt" -eq 0 ]]; then
      echo "[corpus] skip $src_stem $enc (VideoToolbox not available — Darwin + ffmpeg-VT required)"
      continue
    fi
    out_jsonl="$PHASE_A_DIR/per_source/${src_stem}_${enc}.jsonl"
    if [[ -s "$out_jsonl" ]]; then
      echo "[corpus] skip $src_stem $enc (existing $out_jsonl)"
      continue
    fi
    cq_args=()
    for cq in "${CQ_LIST[@]}"; do
      cq_args+=(--cq "$cq")
    done
    t0=$(date -u +%s)
    echo "[corpus] $src_stem $enc cqs=${CQS}"
    python3 "$REPO_ROOT/scripts/dev/hw_encoder_corpus.py" \
      --vmaf-bin "$LIBVMAF_BIN" \
      --source "$yuv" \
      --width "$WIDTH" --height "$HEIGHT" \
      --pix-fmt "$PIX_FMT" --framerate "$FRAMERATE" \
      --encoder "$enc" \
      "${cq_args[@]}" \
      --out "$out_jsonl"
    t1=$(date -u +%s)
    echo "[corpus]   -> $((t1 - t0))s, $(wc -l <"$out_jsonl") rows"
  done
done

# Concatenate per-source shards into the canonical corpus.
canonical="$PHASE_A_DIR/per_frame_canonical6.jsonl"
cat "$PHASE_A_DIR"/per_source/*.jsonl >"$canonical"
echo "[corpus] canonical=$canonical rows=$(wc -l <"$canonical") elapsed=$(($(date -u +%s) - t_total))s"
