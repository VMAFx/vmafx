#!/usr/bin/env bash
#
# scaffold.sh — thin driver for /bisect-model-quality.
#
# Usage: bash .claude/skills/bisect-model-quality/scaffold.sh \
#          --features <path.parquet> --min-plcc <X> | --min-srocc <Y> | --max-rmse <Z> \
#          [--input-name <name>] [--json <out.json>] [--fail-on-first-bad] \
#          -- model_0.onnx model_1.onnx … model_N.onnx
#
# Wraps `vmaf-train bisect-model-quality` (the actual O(log N) ONNX bisector)
# with the operator-tree guards from lib/bisect-common.sh and renders a verdict
# to /tmp/bisect-model-quality-YYYY-MM-DD.md. The script does NOT rebuild
# anything — it only runs ORT inference.

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1091  # sourced relative path resolved at runtime
source "$script_dir/../lib/bisect-common.sh"

passthrough=()
models=()
seen_dash_dash=0

while [[ $# -gt 0 ]]; do
  if [[ $seen_dash_dash -eq 1 ]]; then
    models+=("$1")
    shift
    continue
  fi
  case "$1" in
    --)
      seen_dash_dash=1
      shift
      ;;
    -h | --help)
      grep -E '^# ' "$0" | sed 's/^# //'
      exit 0
      ;;
    *)
      passthrough+=("$1")
      shift
      ;;
  esac
done

if [[ ${#models[@]} -lt 2 ]]; then
  bisect_die "need at least 2 model paths after --; got ${#models[@]}"
fi

# Sanity: every model exists.
for m in "${models[@]}"; do
  [[ -f "$m" ]] || bisect_die "model not found: $m"
done

# We don't checkout anything, but enforce the same clean-tree gate for operator
# predictability — surprises from a half-staged repo are equally bad for either
# bisect flow.
bisect_require_clean_tree

if ! command -v vmaf-train >/dev/null 2>&1; then
  bisect_die "vmaf-train CLI not on PATH; install ai/ first (pip install -e ai/)"
fi

date_stamp=$(date +%Y-%m-%d)
report_md="/tmp/bisect-model-quality-${date_stamp}.md"
report_json="/tmp/bisect-model-quality-${date_stamp}.json"

bisect_log "running: vmaf-train bisect-model-quality ${#models[@]} models, args: ${passthrough[*]:-(none)}"
set +e
vmaf-train bisect-model-quality "${models[@]}" \
  "${passthrough[@]}" \
  --json "$report_json"
rc=$?
set -e

if [[ $rc -ne 0 && $rc -ne 1 ]]; then
  bisect_die "vmaf-train bisect-model-quality failed (rc=$rc)"
fi

# Parse first-bad from the JSON report. Fall back to a string verdict if jq
# is unavailable.
first_bad="(unknown — see JSON report)"
if command -v jq >/dev/null 2>&1 && [[ -s "$report_json" ]]; then
  first_bad=$(jq -r '.verdict // .first_bad // "unknown"' "$report_json" 2>/dev/null || echo "unknown")
fi

# shellcheck disable=SC2016  # backticks are literal markdown code-span markers
extra=$(printf '## Models visited\n\n%d candidates\n\n## JSON report\n\n`%s`\n' \
  "${#models[@]}" "$report_json")
bisect_render_verdict "checkpoint" "$first_bad" "$report_md" "$extra"
