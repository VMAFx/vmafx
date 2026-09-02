#!/usr/bin/env bash
# Wrapper for the Synamedia/Quortex x264-pVMAF predictor.
#
# Upstream: https://github.com/quortex/x264-pVMAF (Nov 2024).
# Upstream license: GPL-2.0.
#
# This fork is BSD-3-Clause-Plus-Patent. To preserve the licence
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

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ref)
      ref="$2"
      shift 2
      ;;
    --dis)
      dis="$2"
      shift 2
      ;;
    --width)
      width="$2"
      shift 2
      ;;
    --height)
      height="$2"
      shift 2
      ;;
    --out)
      out="$2"
      shift 2
      ;;
    --fps)
      fps="$2"
      shift 2
      ;;
    --pixfmt)
      pixfmt="$2"
      shift 2
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      echo "unknown arg: $1" >&2
      usage
      exit 2
      ;;
  esac
done

for v in ref dis width height out; do
  if [[ -z "${!v}" ]]; then
    echo "missing required --$v" >&2
    usage
    exit 2
  fi
done

bin="${EXTERNAL_BENCH_X264_PVMAF:-}"
if [[ -z "$bin" ]]; then
  cat >&2 <<'EOF'
EXTERNAL_BENCH_X264_PVMAF is not set.

This wrapper does not vendor x264-pVMAF (GPL-2.0). Install it
yourself and point the env var at the binary; see the comment block
at the top of this file.
EOF
  exit 3
fi
if [[ ! -x "$bin" ]]; then
  echo "EXTERNAL_BENCH_X264_PVMAF=$bin is not executable" >&2
  exit 3
fi

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

# Normalise to the harness schema. We delegate to a small Python
# helper because shell JSON munging is brittle and the schema lives
# alongside compare.py.
python3 - "$tmp_out" "$out" "x264-pvmaf" <<'PY'
import json, sys, pathlib
src = pathlib.Path(sys.argv[1])
dst = pathlib.Path(sys.argv[2])
competitor = sys.argv[3]

raw = json.loads(src.read_text()) if src.stat().st_size else {}
frames_in = raw.get("frames", [])
frames = []
for i, f in enumerate(frames_in):
    frames.append({
        "frame_idx": int(f.get("frame", f.get("idx", i))),
        "predicted_vmaf_or_mos": float(f.get("predicted_vmaf",
                                              f.get("vmaf", 0.0))),
        "runtime_ms": float(f.get("runtime_ms", 0.0)),
    })
summary = {
    "competitor": competitor,
    "plcc": float(raw.get("plcc", 0.0)),
    "srocc": float(raw.get("srocc", 0.0)),
    "rmse": float(raw.get("rmse", 0.0)),
    "runtime_total_ms": float(raw.get("runtime_total_ms",
                                      sum(f["runtime_ms"] for f in frames))),
    "params": int(raw.get("params", 0)),
    "gflops": float(raw.get("gflops", 0.0)),
}
dst.write_text(json.dumps({"frames": frames, "summary": summary}, indent=2))
PY
