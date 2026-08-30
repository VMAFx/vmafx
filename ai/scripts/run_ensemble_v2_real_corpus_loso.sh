#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# Real-corpus LOSO retrain wrapper for the ``fr_regressor_v2`` deep
# ensemble (ADR-0309 — companion to ADR-0303). The wrapper loops the
# trainer over five seeds against the Phase A canonical-6 per-frame
# JSONL corpus produced by ``scripts/dev/hw_encoder_corpus.py``
# (PR #392).
#
# ADR-0318 fix: the trainer's CLI is the authoritative interface
# (``--corpus`` JSONL path, ``--out-dir``); earlier wrapper revisions
# passed ``--corpus-root`` / ``--output``, which the trainer rejects
# with ``unrecognized arguments``. Operators must produce the
# canonical-6 JSONL before invoking this wrapper — see
# ``docs/ai/ensemble-v2-real-corpus-retrain-runbook.md`` step 0.
#
# Usage:
#   bash ai/scripts/run_ensemble_v2_real_corpus_loso.sh
#   CORPUS_JSONL=/path/per_frame_canonical6.jsonl \
#     bash ai/scripts/run_ensemble_v2_real_corpus_loso.sh
#
# Wall-time estimate: 6-12 h on an 8 GB GPU (5 seeds x 9 LOSO folds),
# plus ~3-5 h for the Phase A pre-step on an RTX 4090 (one-shot, can
# be skipped on subsequent retrains if the JSONL is already present).
#
# After the wrapper finishes, run:
#   python ai/scripts/validate_ensemble_seeds.py runs/ensemble_v2_real/
# to apply the two-part production-flip gate (ADR-0303) and emit
# either runs/ensemble_v2_real/PROMOTE.json or HOLD.json.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root"

corpus_jsonl="${CORPUS_JSONL:-$repo_root/runs/phase_a/full_grid/per_frame_canonical6.jsonl}"
# Informational only — the YUV directory is consumed by the Phase A
# pre-step (``scripts/dev/hw_encoder_corpus.py``), not by the trainer.
corpus_root="${CORPUS_ROOT:-$repo_root/.workingdir2/netflix}"
out_dir="${OUT_DIR:-$repo_root/runs/ensemble_v2_real}"
log_dir="$out_dir/logs"
seeds=(0 1 2 3 4)

if [[ ! -f "$corpus_jsonl" ]]; then
  echo "error: Phase A canonical-6 corpus JSONL not found: $corpus_jsonl" >&2
  echo "Run the Phase A pre-step first — see step 0 of" \
    "docs/ai/ensemble-v2-real-corpus-retrain-runbook.md" \
    "(scripts/dev/hw_encoder_corpus.py over the 9 Netflix sources" \
    "x {h264_nvenc, h264_qsv} x CQs, then concatenate the per-call" \
    "JSONLs into runs/phase_a/full_grid/per_frame_canonical6.jsonl)." >&2
  echo "Override the path with \$CORPUS_JSONL if it lives elsewhere." >&2
  exit 2
fi

# Informational sanity-check on the raw YUV directory; the trainer
# does not consume YUVs directly (the Phase A pre-step does), so a
# missing directory is only a warning here, not a hard failure.
if [[ -d "$corpus_root" ]]; then
  yuv_count=$(find "$corpus_root" -maxdepth 3 -name '*.yuv' 2>/dev/null | wc -l)
  echo "[ensemble-v2-real] (info) raw YUV corpus at $corpus_root: ${yuv_count} files"
else
  echo "[ensemble-v2-real] (info) raw YUV corpus dir not present at $corpus_root —" \
    "fine if the canonical-6 JSONL was produced elsewhere"
fi

mkdir -p "$out_dir" "$log_dir"

ts="$(date -u +%Y%m%dT%H%M%SZ)"
echo "[ensemble-v2-real] corpus_jsonl=$corpus_jsonl"
echo "[ensemble-v2-real] out_dir=$out_dir log_dir=$log_dir start=$ts"

start_secs=$(date -u +%s)

for seed in "${seeds[@]}"; do
  log_file="$log_dir/seed${seed}_${ts}.log"
  echo "[ensemble-v2-real] seed=$seed -> $log_file"
  # Trainer writes loso_seed${seed}.json to --out-dir automatically
  # (see ai/scripts/train_fr_regressor_v2_ensemble_loso.py main()).
  python "$repo_root/ai/scripts/train_fr_regressor_v2_ensemble_loso.py" \
    --seeds "$seed" \
    --corpus "$corpus_jsonl" \
    --out-dir "$out_dir" \
    2>&1 | tee "$log_file"
done

end_secs=$(date -u +%s)
elapsed=$((end_secs - start_secs))

echo "[ensemble-v2-real] done seeds=${seeds[*]} elapsed=${elapsed}s out_dir=$out_dir"
echo "[ensemble-v2-real] next: python ai/scripts/validate_ensemble_seeds.py $out_dir"
