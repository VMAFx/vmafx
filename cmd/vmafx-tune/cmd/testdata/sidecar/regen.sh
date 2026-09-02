#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
#
# Regenerate the `vmaf-tune sidecar` parity fixtures from the in-tree Python
# CLI. TestSidecarPythonParity (../../sidecar_parity_test.go) replays the
# same operator sequence through the Go command tree and requires every
# stdout byte, every state.json snapshot and every exit status to match.
#
# Regenerate ONLY alongside a deliberate, coordinated change on both sides
# (pkg/tune/AGENTS.md "Regenerating the parity fixtures"): a silent
# regeneration turns the parity gate into a tautology.
#
# Determinism: the host UUID is pinned by pre-writing cache/host-uuid, and
# --cache-dir is the RELATIVE path "cache" so state_path renders identically
# on every machine. The Python analytical predictor is used throughout
# (--model is never passed), so onnxruntime is not required.
#
# Usage (from the repository root):
#   bash cmd/vmafx-tune/cmd/testdata/sidecar/regen.sh
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../../../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cd "$WORK"
mkdir -p cache
cp "$HERE/host-uuid" cache/host-uuid
for f in features.json captures.jsonl array.json partial.json broken-features.txt; do
  cp "$HERE/$f" "$f"
done

py() {
  PYTHONPATH="$REPO/tools/vmaf-tune/src" python3 -m vmaftune.cli "$@"
}

# run <name> <args...>: capture stdout and exit status; stderr is discarded
# because diagnostics are not part of the byte-compatibility contract.
run() {
  local name=$1
  shift
  set +e
  py "$@" >"$HERE/$name.out" 2>/dev/null
  echo $? >"$HERE/$name.exit"
  set -e
}
snap() { cp cache/predictor_v1/libx264/state.json "$HERE/$1"; }

run status_cold_json sidecar status --cache-dir cache --json
run status_cold_text sidecar status --cache-dir cache
run predict_cold_json sidecar predict --cache-dir cache --features-json features.json --crf 26 --json
run record_1_json sidecar record --cache-dir cache --features-json features.json --crf 26 --observed-vmaf 91.75 --json
snap state_after_record_1.json
run record_2_text sidecar record --cache-dir cache --features-json features.json --crf 30 --observed-vmaf 88.5
snap state_after_record_2.json
run record_3_int_json sidecar record --cache-dir cache --features-json features.json --crf 22 --observed-vmaf 96 --json
snap state_after_record_3.json
run batch_1_json sidecar batch-record --cache-dir cache --captures-jsonl captures.jsonl --json
snap state_after_batch_1.json
run batch_2_text sidecar batch-record --cache-dir cache --captures-jsonl captures.jsonl
snap state_after_batch_2.json
run status_warm_json sidecar status --cache-dir cache --json
run status_warm_text sidecar status --cache-dir cache
run predict_warm_json sidecar predict --cache-dir cache --features-json features.json --crf 26 --json
run predict_warm_text sidecar predict --cache-dir cache --features-json features.json --crf 26
run record_nopersist_json sidecar record --cache-dir cache --features-json features.json --crf 26 --observed-vmaf 91.75 --no-persist --json
snap state_after_nopersist.json
run status_x265_json sidecar status --cache-dir cache --codec libx265 --predictor-version predictor_v2 --json
# Error paths: only the exit status is pinned (stdout must stay empty).
run err_bad_codec sidecar status --cache-dir cache --codec libnope
run err_missing_required sidecar record --cache-dir cache --features-json features.json --crf 26
run err_bad_crf sidecar predict --cache-dir cache --features-json features.json --crf 26.5
run err_unknown_flag sidecar status --cache-dir cache --bogus
run err_features_missing sidecar predict --cache-dir cache --features-json absent.json --crf 26
run err_features_array sidecar record --cache-dir cache --features-json array.json --crf 26 --observed-vmaf 90
run err_features_partial sidecar predict --cache-dir cache --features-json partial.json --crf 26
run err_features_broken sidecar predict --cache-dir cache --features-json broken-features.txt --crf 26
run err_captures_missing sidecar batch-record --cache-dir cache --captures-jsonl absent.jsonl
ls cache/predictor_v1/libx264/ >"$HERE/state_dir_listing.txt"

{
  echo "python: $(python3 --version 2>&1)"
  echo "commit: $(git -C "$REPO" rev-parse --short HEAD)"
  echo "generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} >"$HERE/PROVENANCE.txt"
echo "fixtures written to $HERE"
