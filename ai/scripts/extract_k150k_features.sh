#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# K150K → FR-corpus extraction runbook (ADR-0346, ADR-0325).
#
# Reads the KonViD-150k JSONL produced by the Phase 2 ingest scripts
# (`ai/scripts/konvid_150k_to_corpus_jsonl.py`), runs each row through
# the FR-from-NR adapter (`vmaftune.fr_from_nr_adapter.NrToFrAdapter`),
# and writes one FR corpus row per (source, preset, CRF) cell into
# the output JSONL. Intended to run **overnight** on a workstation:
# 148,543 K150K rows × 5 CRFs × ~1-3 minutes per encode = 12k+
# wall-clock hours single-stream, parallelism is the operator's call.
#
# This script intentionally has zero dependency-PR coupling: it only
# orchestrates the published adapter API. When PRs #462 / #472 / #477
# / #482 (the K150K ingest stack) merge, the adapter is invocable
# directly via this runbook.
#
# Usage:
#   bash ai/scripts/extract_k150k_features.sh \
#       --input  .workingdir2/konvid-150k/konvid_150k.jsonl \
#       --output runs/k150k_fr_corpus.jsonl
#
# Environment overrides:
#   VMAFTUNE_PRESET    libx264 preset for the FR sweep (default: medium)
#   VMAFTUNE_CRF_SWEEP comma-separated CRFs           (default: 18,23,28,33,38)
#   VMAFTUNE_SCRATCH   raw-YUV intermediate dir       (default: .workingdir2/k150k-scratch)
#   VMAFTUNE_ENCODES   re-encoded MP4 dir             (default: .workingdir2/k150k-encodes)

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root"

input=""
output=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --input)
      input="$2"
      shift 2
      ;;
    --output)
      output="$2"
      shift 2
      ;;
    -h | --help)
      sed -n '1,32p' "${BASH_SOURCE[0]}"
      exit 0
      ;;
    *)
      echo "unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

if [[ -z "$input" || -z "$output" ]]; then
  echo "usage: $0 --input <konvid_150k.jsonl> --output <fr_corpus.jsonl>" >&2
  exit 2
fi

preset="${VMAFTUNE_PRESET:-medium}"
crf_sweep="${VMAFTUNE_CRF_SWEEP:-18,23,28,33,38}"
scratch_dir="${VMAFTUNE_SCRATCH:-$repo_root/.workingdir2/k150k-scratch}"
encode_dir="${VMAFTUNE_ENCODES:-$repo_root/.workingdir2/k150k-encodes}"

mkdir -p "$scratch_dir" "$encode_dir"
mkdir -p "$(dirname "$output")"

echo "[k150k-extract] input          = $input"
echo "[k150k-extract] output         = $output"
echo "[k150k-extract] preset         = $preset"
echo "[k150k-extract] crf_sweep      = $crf_sweep"
echo "[k150k-extract] scratch_dir    = $scratch_dir"
echo "[k150k-extract] encode_dir     = $encode_dir"

PYTHONPATH="$repo_root/tools/vmaf-tune/src" \
  python - "$input" "$output" "$preset" "$crf_sweep" "$scratch_dir" "$encode_dir" <<'PY'
import json
import os
import sys
from pathlib import Path

from vmaftune.corpus import CorpusOptions
from vmaftune.defaultmodel import DEFAULT_MODEL
from vmaftune.fr_from_nr_adapter import NrInputRow, NrToFrAdapter

input_path = Path(sys.argv[1])
output_path = Path(sys.argv[2])
preset = sys.argv[3]
crf_sweep = tuple(int(c) for c in sys.argv[4].split(",") if c.strip())
scratch_dir = Path(sys.argv[5])
encode_dir = Path(sys.argv[6])

adapter = NrToFrAdapter(
    crf_sweep=crf_sweep,
    preset=preset,
    scratch_dir=scratch_dir,
    keep_intermediate_yuv=False,
    options=CorpusOptions(
        encoder="libx264",
        encode_dir=encode_dir,
        keep_encodes=False,
        vmaf_model=DEFAULT_MODEL,
    ),
)

count_in = 0
count_out = 0
with input_path.open("r", encoding="utf-8") as fh_in, output_path.open(
    "w", encoding="utf-8"
) as fh_out:
    for line in fh_in:
        line = line.strip()
        if not line:
            continue
        nr_row = NrInputRow.from_dict(json.loads(line))
        count_in += 1
        try:
            for fr_row in adapter.run(nr_row):
                fh_out.write(json.dumps(fr_row, sort_keys=True))
                fh_out.write(os.linesep)
                count_out += 1
        except RuntimeError as err:
            sys.stderr.write(f"[k150k-extract] skipping {nr_row.src}: {err}\n")
            continue

sys.stderr.write(
    f"[k150k-extract] done: read {count_in} NR rows, wrote {count_out} FR rows\n"
)
PY
