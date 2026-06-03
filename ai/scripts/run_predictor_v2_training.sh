#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# Phase 2 orchestration wrapper for the per-codec predictor v2
# real-corpus trainer. Runs locally on the operator's machine — the
# agent that wrote this script does NOT have the corpora; the operator
# does.
#
# Pipeline:
#   1. Discover what corpus is available under ~/.workingdir2/.
#   2. Run ai/scripts/train_predictor_v2_realcorpus.py to produce a
#      per-codec JSON report (PLCC / SROCC / RMSE per fold + the
#      ADR-0303 gate verdict).
#   3. For codecs that PASS the gate, retrain on the full corpus and
#      overwrite model/predictor_<codec>.onnx + the model card with
#      REAL numbers.
#   4. For codecs that FAIL, leave the shipped stub untouched and
#      append a "Status: Proposed (gate-failed: REASON)" block to the
#      model card. Per CLAUDE.md §13 / feedback_no_test_weakening, the
#      gate is load-bearing and must not be silently lowered.
#
# Usage:
#   bash ai/scripts/run_predictor_v2_training.sh
#   CORPUS=/path/to/corpus.jsonl bash ai/scripts/run_predictor_v2_training.sh
#   CORPUS_ROOTS="/a /b" bash ai/scripts/run_predictor_v2_training.sh
#   ALLOW_EMPTY=1 bash ai/scripts/run_predictor_v2_training.sh
#
# Wall-time estimate: ~5–60 min on a CPU-only host (14 codecs ×
# 5 LOSO folds × ~200 epochs × tiny-MLP). Real-corpus retrains
# scale with row count.
#
# After this finishes, commit:
#   - Updated model/predictor_<codec>.onnx (PASS codecs only)
#   - Updated model/predictor_<codec>_card.md (every codec)
#   - The runs/predictor_v2_realcorpus/report.json (for audit)
# in a follow-up PR. This script does not stage or commit anything;
# the operator reviews the diff first.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root"

# ---------------------------------------------------------------------
# Configuration knobs (env-overridable)
# ---------------------------------------------------------------------

# Specific corpus file overrides discovery roots when set.
corpus="${CORPUS:-}"

# Discovery roots — defaults to the three configured-by-task corpora.
# The trainer's --corpus-root flag is repeatable; we pass each space-
# separated entry through.
default_roots=(
  "$HOME/.workingdir2/netflix"
  "$HOME/.workingdir2/konvid-150k"
  "$HOME/.workingdir2/bvi-dvc-raw"
)
corpus_roots="${CORPUS_ROOTS:-${default_roots[*]}}"

out_dir="${OUT_DIR:-$repo_root/runs/predictor_v2_realcorpus}"
report_path="${REPORT_PATH:-$out_dir/report.json}"
model_dir="${MODEL_DIR:-$repo_root/model}"
epochs="${EPOCHS:-200}"
seed="${SEED:-42}"
allow_empty="${ALLOW_EMPTY:-0}"

mkdir -p "$out_dir"

# ---------------------------------------------------------------------
# Step 1: discover what is on disk
# ---------------------------------------------------------------------

echo "[predictor-v2] repo_root=$repo_root"
echo "[predictor-v2] out_dir=$out_dir"
echo "[predictor-v2] model_dir=$model_dir"

if [[ -n "$corpus" ]]; then
  if [[ ! -f "$corpus" ]]; then
    echo "error: CORPUS=$corpus is not a regular file" >&2
    exit 2
  fi
  echo "[predictor-v2] corpus=$corpus (explicit)"
else
  echo "[predictor-v2] corpus_roots=$corpus_roots"
  any_root=0
  for root in $corpus_roots; do
    if [[ -d "$root" ]]; then
      any_root=1
      n=$(find "$root" -name '*.jsonl' -type f 2>/dev/null | wc -l)
      echo "[predictor-v2]   discovered: $root (${n} jsonl files)"
    else
      echo "[predictor-v2]   missing:    $root"
    fi
  done
  if [[ "$any_root" -eq 0 && "$allow_empty" -ne 1 ]]; then
    echo "" >&2
    echo "error: no corpus roots present on disk." >&2
    echo "Populate at least one of:" >&2
    for root in $corpus_roots; do echo "  - $root" >&2; done
    echo "or set ALLOW_EMPTY=1 for a diagnostic run." >&2
    exit 2
  fi
fi

# ---------------------------------------------------------------------
# Step 2: invoke the trainer
# ---------------------------------------------------------------------

trainer_args=(
  "ai/scripts/train_predictor_v2_realcorpus.py"
  "--epochs" "$epochs"
  "--seed" "$seed"
  "--report-out" "$report_path"
)

if [[ -n "$corpus" ]]; then
  trainer_args+=("--corpus" "$corpus")
else
  for root in $corpus_roots; do
    if [[ -d "$root" ]]; then
      trainer_args+=("--corpus-root" "$root")
    fi
  done
fi

if [[ "$allow_empty" -eq 1 ]]; then
  trainer_args+=("--allow-empty")
fi

log_file="$out_dir/train_$(date -u +%Y%m%dT%H%M%SZ).log"
echo "[predictor-v2] trainer log: $log_file"
echo "[predictor-v2] running:     python ${trainer_args[*]}"

# The trainer exits non-zero if any codec fails the gate; we still
# want to walk the report afterwards (failing codecs need their cards
# updated with a Status: Proposed note).
set +e
python "${trainer_args[@]}" 2>&1 | tee "$log_file"
trainer_rc=${PIPESTATUS[0]}
set -e
echo "[predictor-v2] trainer exit code: $trainer_rc"

if [[ ! -f "$report_path" ]]; then
  echo "error: trainer did not produce a report at $report_path" >&2
  exit 3
fi

# ---------------------------------------------------------------------
# Step 3: per-codec post-processing — update model cards from the JSON
# ---------------------------------------------------------------------
#
# We do this in a tiny inline Python helper to avoid a second script
# file. It reads $report_path and, for each codec, either:
#   - PASS: writes a "Status: Production" header block to the card.
#     The actual ONNX overwrite is done by the trainer module (PR #450)
#     when its full-corpus retrain runs; this orchestrator triggers
#     that retrain for PASS codecs only.
#   - FAIL / insufficient-sources / missing-rows: appends a
#     "Status: Proposed (gate-failed: REASON)" block. Card stays
#     pointing at the synthetic stub ONNX.

python - "$report_path" "$model_dir" "$repo_root" "$epochs" "$seed" <<PYEOF
import json
import subprocess
import sys
from pathlib import Path

report_path, model_dir, repo_root, epochs_str, seed_str = sys.argv[1:6]
report = json.loads(Path(report_path).read_text(encoding="utf-8"))
model_dir = Path(model_dir)
repo_root = Path(repo_root)

epochs = int(epochs_str)
seed = int(seed_str)

# Find the explicit corpus path from the report; we re-pass it to the
# PR #450 trainer module for the full-corpus retrain on PASS codecs.
discovered = report["corpus"]["discovered_files"]


def _retrain_full_corpus(codec: str) -> None:
    """Re-run vmaftune.predictor_train for one codec on the full corpus.

    PR #450's trainer is the canonical ONNX exporter; we invoke it via
    -m so the ONNX byte stream matches exactly what shipped on master.
    The first available corpus file is passed; if more than one root
    contributed rows, a future enhancement could merge them, but per
    Phase-2 scope each operator typically has one canonical corpus.
    """
    if not discovered:
        print(f"  {codec}: no corpus to retrain on; skipping ONNX overwrite")
        return
    corpus = discovered[0]
    cmd = [
        sys.executable,
        "-m", "vmaftune.predictor_train",
        "--corpus", corpus,
        "--output-dir", str(model_dir),
        "--epochs", str(epochs),
        "--seed", str(seed),
        "--codec", codec,
    ]
    env = {
        **{k: v for k, v in __import__("os").environ.items()},
        "PYTHONPATH": str(repo_root / "tools" / "vmaf-tune" / "src")
        + ":" + (__import__("os").environ.get("PYTHONPATH") or ""),
    }
    print(f"  {codec}: retraining on full corpus -> {model_dir}/predictor_{codec}.onnx")
    rc = subprocess.run(cmd, env=env).returncode
    if rc != 0:
        print(f"  {codec}: WARNING — full-corpus retrain failed (rc={rc}); "
              f"shipped stub left untouched")


def _patch_card(codec: str, payload: dict) -> None:
    card = model_dir / f"predictor_{codec}_card.md"
    if not card.is_file():
        print(f"  {codec}: card not found at {card}; skipping patch")
        return
    text = card.read_text(encoding="utf-8")

    status = payload["status"]
    mean_plcc = payload["mean_plcc"]
    spread = payload["plcc_spread"]
    if status == "pass":
        block = (
            "\n## Status: Production (ADR-0303 gate cleared)\n\n"
            f"- LOSO mean PLCC: {mean_plcc:.4f} (>= 0.95)\n"
            f"- LOSO PLCC spread: {spread:.4f} (<= 0.005)\n"
            f"- Folds: {len(payload['folds'])}\n"
            f"- Corpus rows: {payload['n_rows_total']}\n"
            f"- Distinct sources: {payload['n_distinct_sources']}\n"
            f"- Report: $(runs/predictor_v2_realcorpus/report.json)\n"
        )
    else:
        reasons = "; ".join(payload["failure_reasons"]) or "(no fold output)"
        block = (
            f"\n## Status: Proposed (gate-failed: {status})\n\n"
            f"- ADR-0303 gate verdict: FAIL\n"
            f"- Reasons: {reasons}\n"
            f"- LOSO mean PLCC: {mean_plcc:.4f} (gate >= 0.95)\n"
            f"- LOSO PLCC spread: {spread:.4f} (gate <= 0.005)\n"
            f"- Folds emitted: {len(payload['folds'])}\n"
            f"- Corpus rows: {payload['n_rows_total']}\n"
            f"- Distinct sources: {payload['n_distinct_sources']}\n"
            f"- Action: ship more training data or supersede ADR-0303 "
            f"(do NOT silently lower the threshold). Stub ONNX unchanged.\n"
        )

    # Strip any prior auto-generated status block from a previous run;
    # idempotency matters because operators iterate.
    marker = "\n## Status: "
    if marker in text:
        text = text.split(marker, 1)[0].rstrip() + "\n"

    card.write_text(text + block, encoding="utf-8")
    print(f"  {codec}: card patched ({status})")


for payload in report["codecs"]:
    codec = payload["codec"]
    status = payload["status"]
    if status == "pass":
        _retrain_full_corpus(codec)
    _patch_card(codec, payload)

s = report["summary"]
print()
print(f"[predictor-v2] post-processing done: {s['n_pass']} pass, "
      f"{s['n_fail']} fail, {s['n_insufficient']} insufficient-sources, "
      f"{s['n_missing_rows']} missing-rows")
PYEOF

# ---------------------------------------------------------------------
# Step 4: leave the trainer's exit code as the script's exit code
# ---------------------------------------------------------------------
#
# A non-zero rc means at least one codec failed the gate; the
# operator reads runs/predictor_v2_realcorpus/report.json + the
# patched cards to decide what to commit. We do NOT auto-stage
# anything — the operator reviews the diff and opens the follow-up
# PR with the trained-model artefacts.

echo ""
echo "[predictor-v2] next steps:"
echo "  1. Inspect $report_path"
echo "  2. Inspect updated $model_dir/predictor_*_card.md"
echo "  3. For PASS codecs, model/predictor_<codec>.onnx is overwritten"
echo "     with the full-corpus retrain; for FAIL codecs the stub is kept."
echo "  4. Commit the diff in a follow-up PR (this script never stages)."

exit "$trainer_rc"
