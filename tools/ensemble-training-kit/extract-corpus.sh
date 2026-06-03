#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# extract-corpus.sh — contributor-side companion to
# prepare-gdrive-bundle.sh. Decodes the lossless HEVC / FFV1 / AV1
# .mkv files in ./corpus/ back to bit-exact .yuv files, then verifies
# every YUV against the bundled sha256 manifest. After a clean run
# the layout matches what run-full-pipeline.sh expects:
#
#   corpus/<corpus_name>/<source>.yuv   (recovered raw YUV)
#
# Bit-exact verification is non-negotiable: VMAF scores diverge
# silently on a ±1 pixel channel error, and a corrupt download is
# the most common reason a contributor's verdict disagrees with the
# lead user's expected PLCC.

set -Eeuo pipefail

CORPUS_DIR="${CORPUS_DIR:-./corpus}"
KEEP_MKV=0
PARALLEL=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --corpus)
      CORPUS_DIR="$2"
      shift 2
      ;;
    --keep-mkv)
      KEEP_MKV=1
      shift
      ;;
    --parallel)
      PARALLEL="$2"
      shift 2
      ;;
    -h | --help)
      sed -n '2,20p' "$0"
      exit 0
      ;;
    *)
      echo "unknown flag: $1" >&2
      exit 2
      ;;
  esac
done

for bin in ffmpeg sha256sum jq; do
  command -v "$bin" >/dev/null 2>&1 || {
    echo "missing dependency: $bin" >&2
    exit 3
  }
done

if [[ ! -d "$CORPUS_DIR" ]]; then
  echo "corpus dir not found: $CORPUS_DIR" >&2
  echo "did you untar the bundle and cd into the kit root?" >&2
  exit 4
fi

manifest="$CORPUS_DIR/manifest.jsonl"
sha_manifest="$CORPUS_DIR/manifest.sha256"
if [[ ! -f "$manifest" ]] || [[ ! -f "$sha_manifest" ]]; then
  echo "manifest not found in $CORPUS_DIR" >&2
  echo "expected: manifest.jsonl + manifest.sha256" >&2
  exit 5
fi

# ---- decode loop -----------------------------------------------------

decode_one() {
  local row="$1"
  local corpus mkv_rel yuv_rel pix_fmt
  corpus="$(jq -r .corpus <<<"$row")"
  mkv_rel="$(jq -r .mkv <<<"$row")"
  yuv_rel="$(jq -r .yuv <<<"$row")"
  pix_fmt="$(jq -r .pix_fmt <<<"$row")"

  local in_mkv="$CORPUS_DIR/$corpus/$mkv_rel"
  local out_yuv="$CORPUS_DIR/$corpus/$yuv_rel"

  if [[ ! -f "$in_mkv" ]]; then
    echo "missing mkv: $in_mkv (download incomplete?)" >&2
    return 1
  fi
  if [[ -f "$out_yuv" ]] && [[ "$out_yuv" -nt "$in_mkv" ]]; then
    echo "  skip (already decoded): $corpus/$yuv_rel"
    return 0
  fi

  mkdir -p "$(dirname "$out_yuv")"
  echo "  decoding: $corpus/$yuv_rel ($pix_fmt)"
  ffmpeg -y -hide_banner -loglevel warning \
    -i "$in_mkv" \
    -f rawvideo -pix_fmt "$pix_fmt" \
    "$out_yuv"
}

export -f decode_one
export CORPUS_DIR

if [[ "$PARALLEL" -gt 1 ]] && command -v parallel >/dev/null 2>&1; then
  <"$manifest" parallel -j"$PARALLEL" decode_one
else
  while IFS= read -r row; do decode_one "$row"; done <"$manifest"
fi

# ---- verify ----------------------------------------------------------

echo
echo "==> verifying every recovered YUV against manifest.sha256"
fail=0
total=0
while IFS= read -r line; do
  expected="$(awk '{print $1}' <<<"$line")"
  rel="$(awk '{$1=""; sub(/^  */,""); print}' <<<"$line")"
  yuv="$CORPUS_DIR/$rel"
  if [[ ! -f "$yuv" ]]; then
    echo "  MISSING: $rel" >&2
    fail=$((fail + 1))
  else
    actual="$(sha256sum "$yuv" | awk '{print $1}')"
    if [[ "$actual" != "$expected" ]]; then
      echo "  BAD SHA: $rel" >&2
      echo "    expected $expected" >&2
      echo "    got      $actual" >&2
      fail=$((fail + 1))
    fi
  fi
  total=$((total + 1))
done <"$sha_manifest"

if [[ "$fail" -gt 0 ]]; then
  echo "verification FAILED: $fail / $total YUVs are missing or corrupt" >&2
  echo "do not proceed with run-full-pipeline.sh; redownload the bundle." >&2
  exit 6
fi
echo "verified: $total / $total YUVs match manifest.sha256"

# ---- cleanup ---------------------------------------------------------

if [[ "$KEEP_MKV" -eq 0 ]]; then
  echo
  echo "==> removing intermediate .mkv files (saves disk; --keep-mkv to retain)"
  while IFS= read -r row; do
    corpus="$(jq -r .corpus <<<"$row")"
    mkv_rel="$(jq -r .mkv <<<"$row")"
    rm -f "$CORPUS_DIR/$corpus/$mkv_rel"
  done <"$manifest"
fi

echo
echo "extraction done. Run the pipeline next:"
echo "  bash run-full-pipeline.sh --ref-dir $CORPUS_DIR"
