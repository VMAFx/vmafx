#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Wrapper for the fork's `nr_metric_v1` (no-reference) predictor.
#
# Same harness schema as the external-competitor wrappers; calls
# the fork's own `vmaf-tune predict` with the no-reference model.
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: run.sh --dis <yuv> --width <W> --height <H> --out <json>
              [--ref <yuv>] [--engine nr_metric_v1] [--fps <fps>] [--pixfmt <yuv420p>]

NOTE: nr_metric_v1 is no-reference. --ref is accepted for harness
symmetry but ignored.

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
engine="nr_metric_v1"
fps="24"
pixfmt="yuv420p"

# shellcheck source=tools/external-bench/lib/common.sh
# shellcheck disable=SC1091
. "$(dirname "${BASH_SOURCE[0]}")/../lib/common.sh"

parse_wrapper_args 1 "$@"
validate_required_args dis width height out

# `ref` is accepted via --ref for harness symmetry but ignored
# (nr_metric_v1 is no-reference). Reference it explicitly to keep
# the linter happy without a per-line disable directive.
: "${ref:-}"

vmaf_tune="${EXTERNAL_BENCH_VMAF_TUNE:-vmaf-tune}"
model_dir="$(resolve_model_dir)"

case "$engine" in
  nr_metric_v1)
    model="$model_dir/nr_metric_v1.json"
    ;;
  *)
    echo "unsupported engine: $engine (this wrapper handles nr_metric_v1)" >&2
    exit 2
    ;;
esac
check_model_exists "$model"

tmp_out="$(mktemp -t nr-metric-XXXXXX.json)"
trap 'rm -f "$tmp_out"' EXIT

"$vmaf_tune" predict \
  --dis "$dis" \
  --width "$width" \
  --height "$height" \
  --pixfmt "$pixfmt" \
  --fps "$fps" \
  --model "$model" \
  --json "$tmp_out"

normalise_output "$tmp_out" "$out" "fork-nr-metric" "mos"
