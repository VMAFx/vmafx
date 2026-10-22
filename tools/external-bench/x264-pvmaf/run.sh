#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Wrapper for the Synamedia/Quortex x264-pVMAF predictor.
#
# Upstream: https://github.com/quortex/x264-pVMAF (Nov 2024).
# Upstream license: GPL-2.0.
#
# To preserve the licence
# boundary documented in ADR-0332 ("External-competitor benchmark
# wrapper-only architecture") this script:
#
#   * MUST NOT vendor, link, or copy any code from x264-pVMAF.
#   * Invokes the user-installed external binary via `subprocess`.
#   * Reads its stdout / output files and re-emits them in the
#     normalised schema the comparison harness consumes.
#
# Operator install (once, outside the fork tree):
#
#     git clone https://github.com/quortex/x264-pVMAF.git ~/external/x264-pVMAF
#     cd ~/external/x264-pVMAF && make
#     export EXTERNAL_BENCH_X264_PVMAF=~/external/x264-pVMAF/x264-pvmaf
#
# This script never re-distributes the GPL'd binary.
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: run.sh --ref <yuv> --dis <yuv> --width <W> --height <H> --out <json>
              [--fps <fps>] [--pixfmt <yuv420p>]

Environment:
  EXTERNAL_BENCH_X264_PVMAF   Path to the user-installed x264-pVMAF binary
                              (required; no default — the fork ships nothing).
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
validate_required_args ref dis width height out

require_external_binary EXTERNAL_BENCH_X264_PVMAF \
  "This wrapper does not vendor x264-pVMAF (GPL-2.0). Install it
yourself and point the env var at the binary; see the comment block
at the top of this file."
bin="$EXTERNAL_BENCH_X264_PVMAF"

# The harness's compare.py overrides this whole script with a stubbed
# subprocess in unit tests, so the exact CLI shape below only has to
# match the real upstream binary at runtime — not at test time. The
# adapter from upstream stdout to the normalised schema lives in the
# Python helper below; bash here only wires the inputs and forwards
# raw stdout to a temp file.
tmp_out="$(mktemp -t x264-pvmaf-XXXXXX.json)"
trap 'rm -f "$tmp_out"' EXIT

# Real invocation (best-effort; refer to upstream README for exact
# flag set on the version operators install). Captured stderr stays
# attached to the user's terminal for debugging.
"$bin" \
  --ref "$ref" \
  --dis "$dis" \
  --width "$width" \
  --height "$height" \
  --pixfmt "$pixfmt" \
  --fps "$fps" \
  --json "$tmp_out"

normalise_output "$tmp_out" "$out" "x264-pvmaf" "vmaf"
