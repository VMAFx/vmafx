# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Shared helper functions for tools/external-bench competitor wrappers.
#
# shellcheck shell=bash

# shellcheck disable=SC2034
parse_wrapper_args() {
  local has_engine="$1"
  shift
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
        if [[ "$has_engine" -ne 1 ]]; then
          echo "unknown arg: $1" >&2
          usage
          exit 2
        fi
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
}

validate_required_args() {
  local v
  for v in "$@"; do
    if [[ -z "${!v:-}" ]]; then
      echo "missing required --$v" >&2
      usage
      exit 2
    fi
  done
}

resolve_model_dir() {
  if [[ -n "${EXTERNAL_BENCH_MODEL_DIR:-}" ]]; then
    printf '%s\n' "$EXTERNAL_BENCH_MODEL_DIR"
    return 0
  fi
  local lib_dir repo_root
  lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  repo_root="$(cd "$lib_dir/../../.." && pwd)"
  printf '%s/model/tiny\n' "$repo_root"
}

check_model_exists() {
  local model="$1"
  if [[ ! -f "$model" ]]; then
    echo "model not found: $model (set EXTERNAL_BENCH_MODEL_DIR or check repo state)" >&2
    exit 3
  fi
}

require_external_binary() {
  local var_name="$1"
  local hint="$2"
  local bin="${!var_name:-}"

  if [[ -z "$bin" ]]; then
    cat >&2 <<EOF
${var_name} is not set.

${hint}
EOF
    exit 3
  fi
  if [[ ! -x "$bin" ]]; then
    echo "${var_name}=${bin} is not executable" >&2
    exit 3
  fi
}

normalise_output() {
  local src="$1"
  local dst="$2"
  local competitor="$3"
  local metric="${4:-vmaf}"

  python3 - "$src" "$dst" "$competitor" "$metric" <<'PY'
import json, sys, pathlib
src = pathlib.Path(sys.argv[1])
dst = pathlib.Path(sys.argv[2])
competitor = sys.argv[3]
metric = sys.argv[4]

raw = json.loads(src.read_text()) if src.stat().st_size else {}
frames_in = raw.get("frames", raw.get("per_frame", []))
frames = []
for i, f in enumerate(frames_in):
    if metric == "mos":
        score = float(f.get("predicted_mos", f.get("score", 0.0)))
    else:
        score = float(f.get("predicted_vmaf", f.get("vmaf", 0.0)))
    frames.append({
        "frame_idx": int(f.get("frame", f.get("idx", i))),
        "predicted_vmaf_or_mos": score,
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
}
