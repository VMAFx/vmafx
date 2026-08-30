#!/usr/bin/env bash
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
    --engine)
      engine="$2"
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

vmaf_tune="${EXTERNAL_BENCH_VMAF_TUNE:-vmaf-tune}"

# Resolve engine label to in-tree ONNX. The ensemble's per-seed
# members live alongside the index sidecar; vmaf-tune picks them
# up via the index file path.
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../../.." && pwd)"
model_dir="${EXTERNAL_BENCH_MODEL_DIR:-$repo_root/model/tiny}"

case "$engine" in
  fr_regressor_v2_ensemble | fr_regressor_v2_ensemble_v1)
    model="$model_dir/fr_regressor_v2_ensemble_v1.json"
    ;;
  *)
    echo "unsupported engine: $engine (this wrapper handles fr_regressor_v2_ensemble*)" >&2
    exit 2
    ;;
esac
if [[ ! -f "$model" ]]; then
  echo "model not found: $model (set EXTERNAL_BENCH_MODEL_DIR or check repo state)" >&2
  exit 3
fi

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

python3 - "$tmp_out" "$out" "fork-fr-regressor" <<'PY'
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
