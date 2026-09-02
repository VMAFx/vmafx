#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# Orchestrator for the ensemble training kit (ADR-0324).
#
# Chains 01..05 with sane error handling.
#
# Usage:
#   bash tools/ensemble-training-kit/run-full-pipeline.sh \
#       --ref-dir /path/to/netflix/ref \
#       [--encoders h264_nvenc,hevc_nvenc] \
#       [--cqs 19,25,31,37] \
#       [--out-dir /path/to/out] \
#       [--seeds 0,1,2,3,4]
#
# Defaults:
#   --encoders h264_nvenc
#   --cqs      19,25,31,37
#   --out-dir  $REPO_ROOT/runs/ensemble_v2_real
#   --seeds    0,1,2,3,4 (the LOSO wrapper hard-codes the seed list;
#              custom seed lists require editing run_ensemble_v2_real_corpus_loso.sh)

set -euo pipefail

KIT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "$KIT_DIR/../.." && pwd)}"

# shellcheck source=tools/ensemble-training-kit/_platform_detect.sh
# shellcheck disable=SC1091
source "$KIT_DIR/_platform_detect.sh"

REF_DIR=""
# Empty = auto-detect from platform; the operator can still override via
# --encoders. The auto-default is computed AFTER flag parsing so an
# explicit --encoders wins.
ENCODERS=""
CQS="19,25,31,37"
OUT_DIR="$REPO_ROOT/runs/ensemble_v2_real"
SEEDS="0,1,2,3,4"

usage() {
  sed -n '/^# Usage:/,/^$/p' "$0" | sed 's/^# \?//'
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ref-dir)
      REF_DIR="$2"
      shift 2
      ;;
    --encoders)
      ENCODERS="$2"
      shift 2
      ;;
    --cqs)
      CQS="$2"
      shift 2
      ;;
    --out-dir)
      OUT_DIR="$2"
      shift 2
      ;;
    --seeds)
      SEEDS="$2"
      shift 2
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      echo "[pipeline] error: unknown flag '$1'" >&2
      usage
      exit 2
      ;;
  esac
done

if [[ -z "$REF_DIR" ]]; then
  echo "[pipeline] error: --ref-dir is required" >&2
  usage
  exit 2
fi
if [[ ! -d "$REF_DIR" ]]; then
  echo "[pipeline] error: --ref-dir '$REF_DIR' is not a directory" >&2
  exit 2
fi
if [[ -z "$(find "$REF_DIR" -maxdepth 1 -type f -name '*.yuv' -print -quit)" ]]; then
  echo "[pipeline] error: --ref-dir '$REF_DIR' has no *.yuv files" >&2
  exit 2
fi

# Auto-default encoders from platform detection if the operator did not
# supply --encoders. Detection branches: NVIDIA -> *_nvenc, iHD -> *_qsv,
# Darwin -> *_videotoolbox, otherwise libx264.
if [[ -z "$ENCODERS" ]]; then
  ENCODERS="$(detect_default_encoders)"
  echo "[pipeline] auto-detected encoders for platform $(detect_platform): $ENCODERS"
fi

# Hard-coded seed list invariant: run_ensemble_v2_real_corpus_loso.sh
# pins seeds=(0 1 2 3 4). Surface the constraint loudly rather than
# silently dropping the operator's --seeds value.
if [[ "$SEEDS" != "0,1,2,3,4" ]]; then
  echo "[pipeline] warning: the LOSO wrapper hard-codes seeds 0..4;" \
    "your --seeds=$SEEDS is recorded in the manifest but the trainer" \
    "will still use 0..4. Edit ai/scripts/run_ensemble_v2_real_corpus_loso.sh" \
    "if you really need a different list." >&2
fi

export REPO_ROOT REF_DIR ENCODERS CQS OUT_DIR

step() {
  echo
  echo "===================================================================="
  echo "[pipeline] step: $1"
  echo "===================================================================="
}

step "01-prereqs.sh"
bash "$KIT_DIR/01-prereqs.sh"

step "02-generate-corpus.sh"
bash "$KIT_DIR/02-generate-corpus.sh"

step "03-train-loso.sh"
bash "$KIT_DIR/03-train-loso.sh"

step "04-validate.sh"
set +e
bash "$KIT_DIR/04-validate.sh"
validate_rc=$?
set -e
if [[ "$validate_rc" -eq 2 ]]; then
  echo "[pipeline] validate step errored; aborting before bundle" >&2
  exit 2
fi

step "05-bundle-results.sh"
bash "$KIT_DIR/05-bundle-results.sh"

echo
echo "[pipeline] done. verdict-rc=$validate_rc (0=PROMOTE, 1=HOLD)"
exit "$validate_rc"
