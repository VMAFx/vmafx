#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# Operator-run orchestration for multi-corpus aggregated training.
# ADR-0340. Discovers which per-corpus MOS JSONLs are present under
# .workingdir2/, runs ai/scripts/aggregate_corpora.py to produce a
# single unified-scale JSONL, and kicks off the predictor v2
# real-corpus trainer (#487) on the result.
#
# This script is intentionally tolerant of partial corpus availability
# — operators on different machines hold different shards. If only
# Konvid + Netflix are on disk, training proceeds with those; the
# aggregator logs which shards were skipped.
#
# Usage:
#   bash ai/scripts/run_aggregated_training.sh [-- TRAINER_ARGS]
#
# Environment variables:
#   VMAF_AGG_OUT      destination for the unified JSONL
#                     (default: .workingdir2/aggregated/unified_corpus.jsonl)
#   VMAF_AGG_DRY_RUN  if set non-empty, run aggregation but skip the
#                     trainer kick-off (useful for CI / smoke tests)
#   VMAF_AGG_TRAINER  trainer entrypoint
#                     (default: ai/scripts/train_predictor_v2_realcorpus.py)
#
# Exit codes:
#   0 — aggregation (and training, if not dry-run) succeeded.
#   2 — no corpus JSONLs discovered on disk.
#   any non-zero — propagated from the aggregator or the trainer.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root"

agg_out="${VMAF_AGG_OUT:-$repo_root/.workingdir2/aggregated/unified_corpus.jsonl}"
trainer="${VMAF_AGG_TRAINER:-$repo_root/ai/scripts/train_predictor_v2_realcorpus.py}"

# Conventional per-corpus JSONL locations. Order matters for
# deterministic dedup-tie-breaking (first-seen wins on equal MOS
# uncertainty).
declare -a candidate_paths=(
  "$repo_root/.workingdir2/netflix/netflix_public.jsonl"
  "$repo_root/.workingdir2/konvid-1k/konvid_1k.jsonl"
  "$repo_root/.workingdir2/konvid-150k/konvid_150k.jsonl"
  "$repo_root/.workingdir2/lsvq/lsvq.jsonl"
  "$repo_root/.workingdir2/youtube-ugc/youtube_ugc.jsonl"
  "$repo_root/.workingdir2/waterloo-ivc-4k/waterloo_ivc_4k.jsonl"
)

declare -a present_paths=()
declare -a missing_paths=()
for p in "${candidate_paths[@]}"; do
  if [[ -f "$p" ]]; then
    present_paths+=("$p")
  else
    missing_paths+=("$p")
  fi
done

echo "[run_aggregated_training] discovered ${#present_paths[@]} corpus JSONL(s):"
for p in "${present_paths[@]}"; do
  echo "  + $p"
done
if ((${#missing_paths[@]} > 0)); then
  echo "[run_aggregated_training] absent (skipped, this is fine):"
  for p in "${missing_paths[@]}"; do
    echo "  - $p"
  done
fi

if ((${#present_paths[@]} == 0)); then
  echo "error: no per-corpus JSONLs found under .workingdir2/." >&2
  echo "Run at least one ingestion script first (see docs/ai/multi-corpus-aggregation.md)." >&2
  exit 2
fi

mkdir -p "$(dirname "$agg_out")"

echo "[run_aggregated_training] aggregating -> $agg_out"
python "$repo_root/ai/scripts/aggregate_corpora.py" \
  --inputs "${present_paths[@]}" \
  --output "$agg_out"

if [[ -n "${VMAF_AGG_DRY_RUN:-}" ]]; then
  echo "[run_aggregated_training] VMAF_AGG_DRY_RUN set; skipping trainer."
  exit 0
fi

if [[ ! -f "$trainer" ]]; then
  echo "warning: trainer entrypoint $trainer not found on disk." >&2
  echo "  This is expected if PR #487 has not landed yet." >&2
  echo "  Set VMAF_AGG_DRY_RUN=1 to bypass, or pass --trainer via" >&2
  echo "  VMAF_AGG_TRAINER=/path/to/train.py once the trainer lands." >&2
  exit 3
fi

echo "[run_aggregated_training] kicking off $trainer with --corpus $agg_out"
exec python "$trainer" --corpus "$agg_out" "$@"
