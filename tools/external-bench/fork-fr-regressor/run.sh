#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Wrapper for the fork's `fr_regressor_v2_ensemble` predictor.
#
# Unlike the external-competitor wrappers, this one calls the
# fork's own `vmaf-tune` CLI directly — same harness schema so
# `compare.py` can put fork and competitor numbers side-by-side
# in one table.
#
# `vmaf-tune predict` accepts `--model <onnx>` rather than an
# `--engine` enum; the wrapper resolves the engine label to the
# canonical model path under `model/tiny/`.
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: run.sh --ref <yuv> --dis <yuv> --width <W> --height <H> --out <json>
              [--engine fr_regressor_v2_ensemble] [--fps <fps>] [--pixfmt <yuv420p>]

Environment:
  EXTERNAL_BENCH_VMAF_TUNE   Path to the `vmaf-tune` CLI (default:
                             `vmaf-tune` on PATH).
  EXTERNAL_BENCH_MODEL_DIR   Override for the in-tree `model/tiny/`
                             directory (default: auto-detected).
USAGE
}

ref=""
dis=""
width=""
height=""
out=""
engine="fr_regressor_v2_ensemble"
fps="24"
pixfmt="yuv420p"

# shellcheck source=tools/external-bench/lib/common.sh
# shellcheck disable=SC1091
. "$(dirname "${BASH_SOURCE[0]}")/../lib/common.sh"

parse_wrapper_args 1 "$@"
validate_required_args ref dis width height out

vmaf_tune="${EXTERNAL_BENCH_VMAF_TUNE:-vmaf-tune}"
model_dir="$(resolve_model_dir)"

# Resolve engine label to in-tree ONNX. The ensemble's per-seed
# members live alongside the index sidecar; vmaf-tune picks them
# up via the index file path.
case "$engine" in
  fr_regressor_v2_ensemble | fr_regressor_v2_ensemble_v1)
    model="$model_dir/fr_regressor_v2_ensemble_v1.json"
    ;;
  *)
    echo "unsupported engine: $engine (this wrapper handles fr_regressor_v2_ensemble*)" >&2
    exit 2
    ;;
esac
check_model_exists "$model"

tmp_out="$(mktemp -t fr-regressor-XXXXXX.json)"
trap 'rm -f "$tmp_out"' EXIT

"$vmaf_tune" predict \
  --ref "$ref" \
  --dis "$dis" \
  --width "$width" \
  --height "$height" \
  --pixfmt "$pixfmt" \
  --fps "$fps" \
  --model "$model" \
  --json "$tmp_out"

normalise_output "$tmp_out" "$out" "fork-fr-regressor" "vmaf"
