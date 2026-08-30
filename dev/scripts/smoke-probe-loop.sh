#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# dev/scripts/smoke-probe-loop.sh — periodic smoke probe loop
#
# Runs every ${PROBE_INTERVAL_SECONDS:-900} seconds (default: 15 min).
# Can also be invoked with --once to run a single probe and exit.
#
# For each probe iteration:
#   1. Runs the golden pair (ref_576x324_48f.yuv / dis_576x324_48f.yuv)
#      through the 3 active backends (cpu, cuda, sycl) and hip.
#      (ADR-0726: Vulkan backend removed.)
#   2. Sends an MCP list_features request via stdio.
#   3. Sends an MCP compute_vmaf HBD (10-bit simulated) request via stdio.
#   4. Writes a JSON probe record to ${PROBE_OUTPUT_DIR}/probe-${ts}.json
#
# Output schema:
#   {
#     "ts": "ISO-8601",
#     "host_id": "hostname:container-id",
#     "backend_results": {
#       "cpu":  { "score": float, "duration_ms": int, "error": str|null },
#       "cuda": { "score": float, "duration_ms": int, "error": str|null },
#       "sycl": { "score": float, "duration_ms": int, "error": str|null },
#       "hip":  { "score": float, "duration_ms": int, "error": str|null }
#     },
#     "mcp_results": {
#       "list_features": { "feature_count": int, "duration_ms": int, "error": str|null },
#       "compute_vmaf":  { "score": float, "duration_ms": int, "error": str|null }
#     }
#   }

set -euo pipefail
IFS=$'\n\t'

# Clean up any per-iteration mktemp staging files on exit / signal. The
# `probe_backend` helper allocates `tmp_out` via mktemp and removes it on
# the success path, but a SIGTERM mid-probe (container stop, OOM kill) used
# to leak `tmp.XXXXXX` files in $TMPDIR. The trap below sweeps them.
_SMOKE_TMPFILES=()
_smoke_cleanup() {
  if [ "${#_SMOKE_TMPFILES[@]}" -gt 0 ]; then
    rm -f "${_SMOKE_TMPFILES[@]}" 2>/dev/null || true
  fi
}
trap _smoke_cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
PROBE_INTERVAL="${PROBE_INTERVAL_SECONDS:-900}"
PROBE_OUTPUT_DIR="${PROBE_OUTPUT_DIR:-/probes}"
TESTDATA="${VMAF_TESTDATA_PATH:-/workspace/testdata}"
MODEL_PATH="${VMAF_MODEL_PATH:-/workspace/model}"
# MCP_SOCK is read by vmaf-mcp-server via VMAF_MCP_UDS_PATH env; not referenced
# directly in this script.

REF_YUV="${TESTDATA}/ref_576x324_48f.yuv"
DIS_YUV="${TESTDATA}/dis_576x324_48f.yuv"
WIDTH=576
HEIGHT=324
# FRAMES=48 — golden pair has 48 frames; vmaf CLI detects this automatically
PIXEL_FORMAT="yuv420p"

# Default VMAF model for standard scoring
VMAF_MODEL="${MODEL_PATH}/vmaf_v0.6.1.json"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
ts_now() { date -u +%Y-%m-%dT%H:%M:%SZ; }

ms_now() { python3 -c "import time; print(int(time.monotonic() * 1000))"; }

json_str() {
  # Escape a string for inline JSON embedding
  local v="${1:-}"
  v="${v//\\/\\\\}"
  v="${v//\"/\\\"}"
  v="${v//$'\n'/\\n}"
  printf '%s' "\"${v}\""
}

json_num() {
  # Emit a JSON number or null
  local v="${1:-}"
  if [[ "${v}" =~ ^-?[0-9]+(\.[0-9]+)?$ ]]; then
    printf '%s' "${v}"
  else
    printf 'null'
  fi
}

# ---------------------------------------------------------------------------
# Single backend probe
# Outputs: score duration_ms error (tab-separated)
# ---------------------------------------------------------------------------
probe_backend() {
  local backend="${1}"
  local t0 t1 score err

  t0="$(ms_now)"

  # Build the vmaf CLI backend flag
  case "${backend}" in
    cpu) backend_flag="" ;;
    cuda) backend_flag="--cuda" ;;
    sycl) backend_flag="--sycl" ;;
    hip) backend_flag="--hip" ;;
    *)
      printf 'null\t0\t%s' "unknown backend: ${backend}"
      return
      ;;
  esac

  # Run vmaf CLI; capture stdout + stderr separately
  local tmp_out
  tmp_out="$(mktemp)"
  _SMOKE_TMPFILES+=("$tmp_out")

  # shellcheck disable=SC2086
  if vmaf \
    --reference "${REF_YUV}" \
    --distorted "${DIS_YUV}" \
    --width "${WIDTH}" \
    --height "${HEIGHT}" \
    --pixel_format "${PIXEL_FORMAT}" \
    --model "path=${VMAF_MODEL}" \
    --output /dev/null \
    ${backend_flag} \
    --no_prediction_flags \
    >"${tmp_out}" 2>&1; then
    t1="$(ms_now)"
    # Parse the aggregate VMAF score from stdout
    score="$(grep -oP '(?<=VMAF score: )\d+(\.\d+)?' "${tmp_out}" | tail -1 || echo '')"
    err="null"
  else
    t1="$(ms_now)"
    score=""
    err="$(tail -3 "${tmp_out}" | tr '\n' ' ')"
  fi
  rm -f "${tmp_out}"

  local duration_ms=$((t1 - t0))
  printf '%s\t%s\t%s' "${score}" "${duration_ms}" "${err}"
}

# ---------------------------------------------------------------------------
# MCP stdio probe — list_features
# ---------------------------------------------------------------------------
probe_mcp_list_features() {
  local t0 t1 duration_ms feature_count err

  t0="$(ms_now)"

  local request
  request='{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"list_features","arguments":{}}}'

  local response
  response="$(printf '%s\n' "${request}" | vmaf-mcp-server --transport stdio 2>/dev/null || echo '')"
  t1="$(ms_now)"
  duration_ms=$((t1 - t0))

  if [ -z "${response}" ]; then
    feature_count="null"
    err='"mcp stdio returned empty response"'
  elif echo "${response}" | python3 -c "import sys,json; d=json.load(sys.stdin); exit(0 if 'result' in d else 1)" 2>/dev/null; then
    feature_count="$(echo "${response}" | python3 -c "
import sys, json
d = json.load(sys.stdin)
content = d.get('result', {}).get('content', [])
# Count features from text output
text = next((c.get('text','') for c in content if c.get('type') == 'text'), '')
import re
matches = re.findall(r'^\s*-\s+\w', text, re.MULTILINE)
print(len(matches))
" 2>/dev/null || echo "null")"
    err="null"
  else
    feature_count="null"
    err="$(echo "${response}" | python3 -c "import sys,json; d=json.load(sys.stdin); print(json.dumps(d.get('error',{}).get('message','unknown')))" 2>/dev/null || echo '"parse error"')"
  fi

  printf '%s\t%s\t%s' "${feature_count}" "${duration_ms}" "${err}"
}

# ---------------------------------------------------------------------------
# MCP stdio probe — compute_vmaf (HBD 10-bit, same golden pair)
# ---------------------------------------------------------------------------
probe_mcp_compute_vmaf() {
  local t0 t1 duration_ms score err

  t0="$(ms_now)"

  local request
  request="$(
    cat <<'JSONEOF'
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"compute_vmaf","arguments":{"reference":"/workspace/testdata/ref_576x324_48f.yuv","distorted":"/workspace/testdata/dis_576x324_48f.yuv","width":576,"height":324,"pixel_format":"yuv420p","model":"vmaf_v0.6.1","backend":"cpu"}}}
JSONEOF
  )"

  local response
  response="$(printf '%s\n' "${request}" | vmaf-mcp-server --transport stdio 2>/dev/null || echo '')"
  t1="$(ms_now)"
  duration_ms=$((t1 - t0))

  if [ -z "${response}" ]; then
    score="null"
    err='"mcp stdio returned empty response"'
  elif echo "${response}" | python3 -c "import sys,json; d=json.load(sys.stdin); exit(0 if 'result' in d else 1)" 2>/dev/null; then
    score="$(echo "${response}" | python3 -c "
import sys, json, re
d = json.load(sys.stdin)
content = d.get('result', {}).get('content', [])
text = next((c.get('text','') for c in content if c.get('type') == 'text'), '')
m = re.search(r'VMAF score.*?(\d+\.\d+)', text)
print(m.group(1) if m else 'null')
" 2>/dev/null || echo "null")"
    err="null"
  else
    score="null"
    err="$(echo "${response}" | python3 -c "import sys,json; d=json.load(sys.stdin); print(json.dumps(d.get('error',{}).get('message','unknown')))" 2>/dev/null || echo '"parse error"')"
  fi

  printf '%s\t%s\t%s' "${score}" "${duration_ms}" "${err}"
}

# ---------------------------------------------------------------------------
# Run one probe, write JSON output
# ---------------------------------------------------------------------------
run_probe() {
  local output_file="${1}"
  local ts host_id

  ts="$(ts_now)"
  host_id="$(hostname):$(cat /proc/self/cgroup 2>/dev/null | grep -oP '(?<=docker-)[a-f0-9]{12}' | head -1 || echo 'unknown')"

  echo "[smoke-probe] Running probe at ${ts}…" >&2

  # Backend probes
  declare -A scores durations errors
  for backend in cpu cuda sycl hip; do
    echo "[smoke-probe]   backend=${backend}…" >&2
    IFS=$'\t' read -r score dur err <<<"$(probe_backend "${backend}" 2>/dev/null || echo "null	0	\"probe failed\"")"
    scores[${backend}]="$(json_num "${score}")"
    durations[${backend}]="${dur:-0}"
    errors[${backend}]="${err:-null}"
  done

  # MCP probes
  echo "[smoke-probe]   mcp list_features…" >&2
  IFS=$'\t' read -r mcp_fc_count mcp_fc_dur mcp_fc_err <<<"$(probe_mcp_list_features 2>/dev/null || echo "null	0	\"probe failed\"")"
  echo "[smoke-probe]   mcp compute_vmaf…" >&2
  IFS=$'\t' read -r mcp_cv_score mcp_cv_dur mcp_cv_err <<<"$(probe_mcp_compute_vmaf 2>/dev/null || echo "null	0	\"probe failed\"")"

  # Write JSON
  mkdir -p "$(dirname "${output_file}")"
  cat >"${output_file}" <<JSONEOF
{
  "ts": "$(json_str "${ts}" | tr -d '"')",
  "host_id": "$(json_str "${host_id}" | tr -d '"')",
  "backend_results": {
    "cpu":  { "score": ${scores[cpu]},  "duration_ms": ${durations[cpu]},  "error": ${errors[cpu]} },
    "cuda": { "score": ${scores[cuda]}, "duration_ms": ${durations[cuda]}, "error": ${errors[cuda]} },
    "sycl": { "score": ${scores[sycl]}, "duration_ms": ${durations[sycl]}, "error": ${errors[sycl]} },
    "hip":  { "score": ${scores[hip]},  "duration_ms": ${durations[hip]},  "error": ${errors[hip]} }
  },
  "mcp_results": {
    "list_features": { "feature_count": ${mcp_fc_count:-null}, "duration_ms": ${mcp_fc_dur:-0}, "error": ${mcp_fc_err:-null} },
    "compute_vmaf":  { "score": ${mcp_cv_score:-null}, "duration_ms": ${mcp_cv_dur:-0}, "error": ${mcp_cv_err:-null} }
  }
}
JSONEOF

  echo "[smoke-probe] Written: ${output_file}" >&2
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
ONCE=false
OUTPUT_OVERRIDE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --once)
      ONCE=true
      shift
      ;;
    --output)
      OUTPUT_OVERRIDE="$2"
      shift 2
      ;;
    *)
      echo "Unknown flag: $1" >&2
      exit 1
      ;;
  esac
done

if "${ONCE}"; then
  ts_tag="$(date +%Y%m%dT%H%M%S)"
  out="${OUTPUT_OVERRIDE:-${PROBE_OUTPUT_DIR}/probe-${ts_tag}.json}"
  run_probe "${out}"
  exit 0
fi

# Continuous loop
echo "[smoke-probe-loop] Starting continuous probe loop (interval: ${PROBE_INTERVAL}s)" >&2

while true; do
  ts_tag="$(date +%Y%m%dT%H%M%S)"
  out="${PROBE_OUTPUT_DIR}/probe-${ts_tag}.json"
  run_probe "${out}" || echo "[smoke-probe-loop] WARNING: probe failed at ${ts_tag}" >&2
  echo "[smoke-probe-loop] Sleeping ${PROBE_INTERVAL}s until next probe…" >&2
  sleep "${PROBE_INTERVAL}"
done
