#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# Step 03: Run the 5-seed x 9-fold LOSO retrain.
#
# Thin pass-through to ai/scripts/run_ensemble_v2_real_corpus_loso.sh.
# Forwards $OUT_DIR + $CORPUS_JSONL through the environment so the
# operator can override either without learning the wrapper's argv.

set -euo pipefail

KIT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "$KIT_DIR/../.." && pwd)}"

OUT_DIR="${OUT_DIR:-$REPO_ROOT/runs/ensemble_v2_real}"
CORPUS_JSONL="${CORPUS_JSONL:-$REPO_ROOT/runs/phase_a/full_grid/per_frame_canonical6.jsonl}"

if [[ ! -f "$CORPUS_JSONL" ]]; then
  echo "[loso] error: corpus not found at $CORPUS_JSONL" >&2
  echo "[loso] hint: run 02-generate-corpus.sh first" >&2
  exit 2
fi

echo "[loso] corpus=$CORPUS_JSONL out_dir=$OUT_DIR"
echo "[loso] invoking ai/scripts/run_ensemble_v2_real_corpus_loso.sh"

CORPUS_JSONL="$CORPUS_JSONL" OUT_DIR="$OUT_DIR" \
  bash "$REPO_ROOT/ai/scripts/run_ensemble_v2_real_corpus_loso.sh"

echo "[loso] done; per-seed JSONs at $OUT_DIR/loso_seed{0..4}.json"
