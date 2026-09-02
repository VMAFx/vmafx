#!/usr/bin/env bash
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

for v in dis width height out; do
  if [[ -z "${!v}" ]]; then
    echo "missing required --$v" >&2
    usage
    exit 2
  fi
done

# `ref` is accepted via --ref for harness symmetry but ignored
# (DOVER-Mobile is no-reference). Reference it explicitly to keep
# the linter happy without a per-line disable directive.
: "${ref:-}"

bin="${EXTERNAL_BENCH_DOVER_MOBILE:-}"
if [[ -z "$bin" ]]; then
  cat >&2 <<'EOF'
EXTERNAL_BENCH_DOVER_MOBILE is not set.

This wrapper does not vendor DOVER-Mobile. Install it via pipx /
uv (see the comment block at the top of this file) and export
the env var.
EOF
  exit 3
fi
if [[ ! -x "$bin" ]]; then
  echo "EXTERNAL_BENCH_DOVER_MOBILE=$bin is not executable" >&2
  exit 3
fi

tmp_out="$(mktemp -t dover-mobile-XXXXXX.json)"
trap 'rm -f "$tmp_out"' EXIT

"$bin" \
  --input "$dis" \
  --width "$width" \
  --height "$height" \
  --pixfmt "$pixfmt" \
  --fps "$fps" \
  --json "$tmp_out"

python3 - "$tmp_out" "$out" "dover-mobile" <<'PY'
import json, sys, pathlib
src = pathlib.Path(sys.argv[1])
dst = pathlib.Path(sys.argv[2])
competitor = sys.argv[3]

raw = json.loads(src.read_text()) if src.stat().st_size else {}
frames_in = raw.get("frames", raw.get("per_frame", []))
frames = []
for i, f in enumerate(frames_in):
    frames.append({
        "frame_idx": int(f.get("frame", f.get("idx", i))),
        "predicted_vmaf_or_mos": float(f.get("predicted_mos",
                                              f.get("score", 0.0))),
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
