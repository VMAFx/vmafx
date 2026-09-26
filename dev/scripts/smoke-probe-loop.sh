#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
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
#   2. Sends an MCP list_extractors request via stdio.
#   3. Sends an MCP vmaf_score request for the same 8-bit pair via stdio.
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

REF_YUV="${TESTDATA}/ref_576x324_48f.yuv"
DIS_YUV="${TESTDATA}/dis_576x324_48f.yuv"
WIDTH=576
HEIGHT=324
# FRAMES=48 — golden pair has 48 frames; vmaf CLI detects this automatically
PIXEL_FORMAT="420"
BITDEPTH=8

# Bash treats tab as IFS whitespace and discards a leading empty field. Use a
# non-whitespace separator so a failed probe cannot shift duration/error into
# the score/duration columns. Error strings are JSON-encoded before they reach
# this boundary, so an actual unit separator is escaped as \u001f.
RESULT_SEP=$'\x1f'

# Default VMAF model for standard scoring
VMAF_MODEL="${MODEL_PATH}/vmaf_v0.6.1.json"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
ts_now() { date -u +%Y-%m-%dT%H:%M:%SZ; }

ms_now() { python3 -c "import time; print(int(time.monotonic() * 1000))"; }

json_str() {
  # Use the same strict JSON encoder as the probe readers. Shell replacement
  # missed tabs and other control bytes, which made an error message capable
  # of corrupting the complete probe record.
  python3 -c 'import json, sys; print(json.dumps(sys.argv[1]), end="")' "${1:-}"
}

json_num() {
  # Emit a JSON number or null
  local v="${1:-}"
  if [[ "${v}" =~ ^-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?$ ]]; then
    printf '%s' "${v}"
  else
    printf 'null'
  fi
}

probe_failed_record() {
  printf 'null%s0%s%s' "${RESULT_SEP}" "${RESULT_SEP}" \
    "$(json_str "probe failed")"
}

# ---------------------------------------------------------------------------
# Single backend probe
# Outputs: score, duration_ms, and JSON-encoded error (unit-separator-delimited)
# ---------------------------------------------------------------------------
probe_backend() {
  local backend="${1}"
  local t0 t1 score err

  t0="$(ms_now)"

  # Reject unknown values before invoking the CLI. Every supported value is
  # passed through the exclusive selector so a successful probe proves that
  # exact backend ran rather than silently falling back through auto dispatch.
  case "${backend}" in
    cpu | cuda | sycl | hip) ;;
    *)
      printf 'null%s0%s%s' "${RESULT_SEP}" "${RESULT_SEP}" \
        "$(json_str "unknown backend: ${backend}")"
      return
      ;;
  esac

  # The CLI reports pooled scores in its JSON output file. It deliberately
  # suppresses the human pooled-score line on non-TTY stderr, so grepping the
  # redirected process output can never be a valid probe. Keep the JSON and
  # diagnostic log separate and verify backend_used as well as the score.
  local tmp_json tmp_log
  tmp_json="$(mktemp)"
  tmp_log="$(mktemp)"
  _SMOKE_TMPFILES+=("${tmp_json}" "${tmp_log}")

  if vmaf \
    --reference "${REF_YUV}" \
    --distorted "${DIS_YUV}" \
    --width "${WIDTH}" \
    --height "${HEIGHT}" \
    --pixel_format "${PIXEL_FORMAT}" \
    --bitdepth "${BITDEPTH}" \
    --model "path=${VMAF_MODEL}" \
    --backend "${backend}" \
    --json \
    --output "${tmp_json}" \
    >"${tmp_log}" 2>&1; then
    t1="$(ms_now)"
    if score="$(
      python3 - "${tmp_json}" "${backend}" 2>>"${tmp_log}" <<'PY'
import json
import math
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    payload = json.load(stream)
score = payload["pooled_metrics"]["vmaf"]["mean"]
if isinstance(score, bool) or not isinstance(score, (int, float)) or not math.isfinite(score):
    raise ValueError(f"non-finite or non-numeric pooled VMAF score: {score!r}")
backend_used = payload.get("backend_used")
if backend_used != sys.argv[2]:
    raise ValueError(f"requested backend {sys.argv[2]!r}, output reports {backend_used!r}")
print(score)
PY
    )"; then
      err="null"
    else
      score="null"
      err="$(json_str "$(tail -3 "${tmp_log}" | tr '\n' ' ')")"
    fi
  else
    t1="$(ms_now)"
    score="null"
    err="$(json_str "$(tail -3 "${tmp_log}" | tr '\n' ' ')")"
  fi
  rm -f "${tmp_json}" "${tmp_log}"

  local duration_ms=$((t1 - t0))
  printf '%s%s%s%s%s' "${score}" "${RESULT_SEP}" "${duration_ms}" \
    "${RESULT_SEP}" "${err}"
}

# Run one MCP tool call over the production Go stdio server. MCP requires an
# initialize handshake before tools/call; a bare tools/call was rejected by the
# Go SDK even when the tool name happened to exist in the retired Python server.
_mcp_call() {
  local tool_name="$1" arguments_json="$2"

  python3 - "${tool_name}" "${arguments_json}" <<'PY'
import json
import os
import selectors
import subprocess
import sys
import time

tool = sys.argv[1]
arguments = json.loads(sys.argv[2])
messages = [
    {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "vmafx-smoke-probe", "version": "1"},
        },
    },
    {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}},
    {
        "jsonrpc": "2.0",
        "id": 2,
        "method": "tools/call",
        "params": {"name": tool, "arguments": arguments},
    },
]

env = os.environ.copy()
env["VMAFX_MCP_TRANSPORT"] = "stdio"
proc = subprocess.Popen(
    ["vmafx-mcp"],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.DEVNULL,
    env=env,
)
response = None
try:
    assert proc.stdin is not None
    assert proc.stdout is not None
    wire = b"".join(
        json.dumps(message, separators=(",", ":")).encode("utf-8") + b"\n"
        for message in messages
    )
    proc.stdin.write(wire)
    proc.stdin.flush()

    # Keep stdin open until the response arrives. Closing it immediately after
    # writing races the Go SDK: EOF disconnects the stdio session before its
    # handler goroutine can publish id=2.
    deadline = time.monotonic() + 120.0
    pending = b""
    with selectors.DefaultSelector() as selector:
        selector.register(proc.stdout, selectors.EVENT_READ)
        while response is None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"MCP tool {tool!r} timed out")
            if not selector.select(remaining):
                raise TimeoutError(f"MCP tool {tool!r} timed out")
            chunk = os.read(proc.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError(
                    f"MCP server exited before replying to tool {tool!r}"
                )
            pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                try:
                    message = json.loads(line)
                except (UnicodeDecodeError, json.JSONDecodeError):
                    continue
                if message.get("id") == 2:
                    response = message
                    break
finally:
    if proc.stdin is not None:
        try:
            proc.stdin.close()
        except OSError:
            pass
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

if response is None:
    raise RuntimeError(f"MCP tool {tool!r} returned no response")
print(json.dumps(response, separators=(",", ":")))
PY
}

# ---------------------------------------------------------------------------
# MCP stdio probe — list_extractors (stable output key: list_features)
# ---------------------------------------------------------------------------
probe_mcp_list_features() {
  local t0 t1 duration_ms feature_count err

  t0="$(ms_now)"

  local response
  response="$(_mcp_call "list_extractors" '{}' || echo '')"
  t1="$(ms_now)"
  duration_ms=$((t1 - t0))

  if [ -z "${response}" ]; then
    feature_count="null"
    err='"mcp stdio returned empty response"'
  elif echo "${response}" | python3 -c "import sys,json; d=json.load(sys.stdin); exit(0 if 'result' in d else 1)" 2>/dev/null; then
    if feature_count="$(echo "${response}" | python3 -c "
import sys, json
d = json.load(sys.stdin)
result = d.get('result', {})
if result.get('isError'):
    raise ValueError('list_extractors returned isError')
content = result.get('content', [])
text = next((c.get('text', '') for c in content if c.get('type') == 'text'), '')
payload = json.loads(text)
extractors = payload.get('extractors')
if not isinstance(extractors, list):
    raise ValueError('list_extractors response lacks an extractors array')
print(len(extractors))
" 2>/dev/null)"; then
      err="null"
    else
      feature_count="null"
      err='"invalid list_extractors response"'
    fi
  else
    feature_count="null"
    err="$(echo "${response}" | python3 -c "import sys,json; d=json.load(sys.stdin); print(json.dumps(d.get('error',{}).get('message','unknown')))" 2>/dev/null || echo '"parse error"')"
  fi

  printf '%s%s%s%s%s' "${feature_count}" "${RESULT_SEP}" "${duration_ms}" \
    "${RESULT_SEP}" "${err}"
}

# ---------------------------------------------------------------------------
# MCP stdio probe — vmaf_score (stable output key: compute_vmaf)
# ---------------------------------------------------------------------------
probe_mcp_compute_vmaf() {
  local t0 t1 duration_ms score err

  t0="$(ms_now)"

  local arguments
  arguments="$(python3 -c '
import json
import sys

print(json.dumps({
    "ref": sys.argv[1],
    "dis": sys.argv[2],
    "width": 576,
    "height": 324,
    "pixfmt": "420",
    "bitdepth": 8,
    "model": "version=vmaf_v0.6.1",
    "backend": "cpu",
}, separators=(",", ":")))
' "${REF_YUV}" "${DIS_YUV}")"

  local response
  response="$(_mcp_call "vmaf_score" "${arguments}" || echo '')"
  t1="$(ms_now)"
  duration_ms=$((t1 - t0))

  if [ -z "${response}" ]; then
    score="null"
    err='"mcp stdio returned empty response"'
  elif echo "${response}" | python3 -c "import sys,json; d=json.load(sys.stdin); exit(0 if 'result' in d else 1)" 2>/dev/null; then
    if score="$(echo "${response}" | python3 -c "
import sys, json, math
d = json.load(sys.stdin)
result = d.get('result', {})
if result.get('isError'):
    raise ValueError('vmaf_score returned isError')
content = result.get('content', [])
text = next((c.get('text', '') for c in content if c.get('type') == 'text'), '')
payload = json.loads(text)
score = payload['pooled_metrics']['vmaf']['mean']
if isinstance(score, bool) or not isinstance(score, (int, float)) or not math.isfinite(score):
    raise ValueError('vmaf_score returned a non-finite score')
if payload.get('backend_used') != 'cpu':
    raise ValueError('vmaf_score did not confirm the CPU backend')
print(score)
" 2>/dev/null)"; then
      err="null"
    else
      score="null"
      err='"invalid vmaf_score response"'
    fi
  else
    score="null"
    err="$(echo "${response}" | python3 -c "import sys,json; d=json.load(sys.stdin); print(json.dumps(d.get('error',{}).get('message','unknown')))" 2>/dev/null || echo '"parse error"')"
  fi

  printf '%s%s%s%s%s' "${score}" "${RESULT_SEP}" "${duration_ms}" \
    "${RESULT_SEP}" "${err}"
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
    IFS="${RESULT_SEP}" read -r score dur err <<<"$(
      probe_backend "${backend}" 2>/dev/null ||
        probe_failed_record
    )"
    scores[${backend}]="$(json_num "${score}")"
    durations[${backend}]="${dur:-0}"
    errors[${backend}]="${err:-null}"
  done

  # MCP probes
  echo "[smoke-probe]   mcp list_extractors…" >&2
  IFS="${RESULT_SEP}" read -r mcp_fc_count mcp_fc_dur mcp_fc_err <<<"$(
    probe_mcp_list_features 2>/dev/null ||
      probe_failed_record
  )"
  echo "[smoke-probe]   mcp vmaf_score…" >&2
  IFS="${RESULT_SEP}" read -r mcp_cv_score mcp_cv_dur mcp_cv_err <<<"$(
    probe_mcp_compute_vmaf 2>/dev/null ||
      probe_failed_record
  )"

  # Write JSON
  mkdir -p "$(dirname "${output_file}")"
  cat >"${output_file}" <<JSONEOF
{
  "ts": $(json_str "${ts}"),
  "host_id": $(json_str "${host_id}"),
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
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
  return 0
fi

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
