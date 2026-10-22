#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Wrapper for DOVER-Mobile (no-reference video quality predictor).
#
# Upstream: https://github.com/QualityAssessment/DOVER (CC-BY-NC-SA
# 4.0 weights; Apache-2.0 code). The mobile variant ships as the
# `dover-mobile` Python package.
#
# Operator install (once, outside the fork tree):
#
#     pipx install dover-mobile
#     # or:
#     uv tool install dover-mobile
#     export EXTERNAL_BENCH_DOVER_MOBILE="$(which dover-mobile)"
#
# As with the x264-pVMAF wrapper, this script does NOT vendor or
# copy DOVER weights or code. It invokes the operator-installed CLI
# and re-shapes its output into the harness schema. See ADR-0332.
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: run.sh --dis <yuv> --width <W> --height <H> --out <json>
              [--ref <yuv>] [--fps <fps>] [--pixfmt <yuv420p>]

NOTE: DOVER-Mobile is no-reference; --ref is accepted for harness
symmetry but ignored.

Environment:
  EXTERNAL_BENCH_DOVER_MOBILE  Path to the user-installed dover-mobile
                               CLI (required; no fork-shipped default).
USAGE
}

ref=""
dis=""
width=""
height=""
out=""
fps="24"
pixfmt="yuv420p"

# shellcheck source=tools/external-bench/lib/common.sh
# shellcheck disable=SC1091
. "$(dirname "${BASH_SOURCE[0]}")/../lib/common.sh"

parse_wrapper_args 0 "$@"
validate_required_args dis width height out

# `ref` is accepted via --ref for harness symmetry but ignored
# (DOVER-Mobile is no-reference). Reference it explicitly to keep
# the linter happy without a per-line disable directive.
: "${ref:-}"

require_external_binary EXTERNAL_BENCH_DOVER_MOBILE \
  "This wrapper does not vendor DOVER-Mobile. Install it via pipx /
uv (see the comment block at the top of this file) and export
the env var."
bin="$EXTERNAL_BENCH_DOVER_MOBILE"

tmp_out="$(mktemp -t dover-mobile-XXXXXX.json)"
trap 'rm -f "$tmp_out"' EXIT

"$bin" \
  --input "$dis" \
  --width "$width" \
  --height "$height" \
  --pixfmt "$pixfmt" \
  --fps "$fps" \
  --json "$tmp_out"

normalise_output "$tmp_out" "$out" "dover-mobile" "mos"
