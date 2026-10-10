#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# dev/scripts/smoke-probe-loop.sh — periodic smoke probe loop
#
# Runs every ${PROBE_INTERVAL_SECONDS:-900} seconds (default: 15 min).
# Can also be invoked with --once to run a single probe and exit: status 0
# when every sub-check passed, 1 when one failed (the record is written
# either way). --healthy reads the newest record instead of probing: status 0
# while it is recent and holds no error; the compose healthcheck calls it.
#
# For each probe iteration:
#   1. Runs the golden pair (ref_576x324_48f.yuv / dis_576x324_48f.yuv)
#      through the backends this host declares in PROBE_BACKENDS (default:
#      cpu cuda sycl hip; ADR-0726: Vulkan backend removed). A declared
#      backend that fails fails the probe; a backend left out is not run and
#      its entry says "probed": false.
#   2. Sends an MCP list_extractors request via stdio.
#   3. Sends an MCP vmaf_score request for the same 8-bit pair via stdio.
#   4. Writes a JSON probe record to ${PROBE_OUTPUT_DIR}/probe-${ts}.json
#
# Output schema:
#   {
#     "ts": "ISO-8601",
#     "host_id": "hostname:container-id",
#     "backend_results": {
#       "cpu":  { "probed": bool, "score": float, "duration_ms": int, "error": str|null },
#       "cuda": { "probed": bool, "score": float, "duration_ms": int, "error": str|null },
#       "sycl": { "probed": bool, "score": float, "duration_ms": int, "error": str|null },
#       "hip":  { "probed": bool, "score": float, "duration_ms": int, "error": str|null }
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
    if ! rm -f "${_SMOKE_TMPFILES[@]}" 2>/dev/null; then
      echo "[smoke-probe] warning: could not remove staging files" >&2
    fi
  fi
}
trap _smoke_cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
PROBE_INTERVAL="${PROBE_INTERVAL_SECONDS:-900}"
# Scalar bound of the continuous loop (HISS-02): centuries at the default 900 s
# interval, so a container never reaches it, but the loop has a bound.
PROBE_MAX_CYCLES="${PROBE_MAX_CYCLES:-1000000000}"
PROBE_OUTPUT_DIR="${PROBE_OUTPUT_DIR:-/probes}"
# The backends this host has. Every one named here must score; leave out the
# ones whose device the host lacks instead of reading their errors as normal.
PROBE_BACKENDS="${PROBE_BACKENDS:-cpu cuda sycl hip}"
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

# True when PROBE_BACKENDS names the backend. An unknown name is a usage error.
backend_declared() {
  local wanted="$1" name names
  IFS=' ,' read -r -a names <<<"${PROBE_BACKENDS}"
  for name in "${names[@]}"; do
    case "${name}" in
      cpu | cuda | sycl | hip) ;;
      *)
        echo "[smoke-probe] PROBE_BACKENDS names an unknown backend: ${name}" >&2
        exit 2
        ;;
    esac
    [ "${name}" != "${wanted}" ] || return 0
  done
  return 1
}

probe_failed_record() {
  printf 'null%s0%s%s' "${RESULT_SEP}" "${RESULT_SEP}" \
    "$(json_str "probe failed")"
}

# Python helpers live in variables so the shell functions below stay short.
# Each program reads its arguments from sys.argv exactly as before.
_SCORE_PY="$(
  cat <<'PY'
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
)"

_MCP_CALL_PY="$(
  cat <<'PY'
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
)"

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
    if score="$(python3 -c "${_SCORE_PY}" "${tmp_json}" "${backend}" 2>>"${tmp_log}")"; then
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
#
# The server refuses a path outside its allowlist (pkg/libvmaf/paths.go). The
# repository roots are allowed only when the server starts inside a checkout,
# and in the container it starts in /build/vmaf, so the golden pair under
# ${TESTDATA} was refused and every vmaf_score probe failed. The probe declares
# that one directory for the server process it starts; the allowlist of a
# server anyone else starts is unchanged.
_mcp_call() {
  local tool_name="$1" arguments_json="$2"

  VMAF_MCP_ALLOW="${TESTDATA}${VMAF_MCP_ALLOW:+:${VMAF_MCP_ALLOW}}" \
    python3 -c "${_MCP_CALL_PY}" "${tool_name}" "${arguments_json}"
}

# Reads one tools/call response on stdin and prints the value named by
# argv[1] (extractors | score). On a refusal or a malformed payload it prints
# the reason instead (the server's own text for isError) and exits 1.
_MCP_RESULT_PY="$(
  cat <<'PY'
import json
import math
import sys

MAX_REASON = 300


def value(kind: str, result: dict) -> object:
    content = result.get("content", [])
    text = next((c.get("text", "") for c in content if c.get("type") == "text"), "")
    if result.get("isError"):
        raise ValueError(text[:MAX_REASON] or "isError without a message")
    payload = json.loads(text)
    if kind == "extractors":
        extractors = payload.get("extractors")
        if not isinstance(extractors, list):
            raise ValueError("response lacks an extractors array")
        return len(extractors)
    score = payload["pooled_metrics"]["vmaf"]["mean"]
    if isinstance(score, bool) or not isinstance(score, (int, float)) or not math.isfinite(score):
        raise ValueError("non-finite score")
    if payload.get("backend_used") != "cpu":
        raise ValueError("response does not confirm the CPU backend")
    return score


try:
    print(value(sys.argv[1], json.load(sys.stdin).get("result", {})))
except (ValueError, KeyError, TypeError, AttributeError) as exc:
    print(f"{type(exc).__name__}: {exc}")
    sys.exit(1)
PY
)"

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
    if feature_count="$(echo "${response}" | python3 -c "${_MCP_RESULT_PY}" extractors 2>/dev/null)"; then
      err="null"
    else
      err="$(json_str "invalid list_extractors response: ${feature_count}")"
      feature_count="null"
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
    if score="$(echo "${response}" | python3 -c "${_MCP_RESULT_PY}" score 2>/dev/null)"; then
      err="null"
    else
      err="$(json_str "invalid vmaf_score response: ${score}")"
      score="null"
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
# Results of the probe in progress; one probe runs at a time.
declare -A scores durations errors probed

# Fill the result tables for the four backends.
probe_backends() {
  local backend score dur err
  for backend in cpu cuda sycl hip; do
    if ! backend_declared "${backend}"; then
      echo "[smoke-probe]   backend=${backend}: not in PROBE_BACKENDS, not probed" >&2
      scores[${backend}]="null" durations[${backend}]=0 errors[${backend}]="null"
      probed[${backend}]=false
      continue
    fi
    probed[${backend}]=true
    echo "[smoke-probe]   backend=${backend}…" >&2
    IFS="${RESULT_SEP}" read -r score dur err <<<"$(
      probe_backend "${backend}" 2>/dev/null ||
        probe_failed_record
    )"
    scores[${backend}]="$(json_num "${score}")"
    durations[${backend}]="${dur:-0}"
    errors[${backend}]="${err:-null}"
  done
}

# Run both MCP checks into mcp_fc_* and mcp_cv_*.
probe_mcp() {
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
}

write_record() {
  local output_file="$1" ts="$2" host_id="$3"

  mkdir -p "$(dirname "${output_file}")"
  cat >"${output_file}" <<JSONEOF
{
  "ts": $(json_str "${ts}"),
  "host_id": $(json_str "${host_id}"),
  "backend_results": {
    "cpu":  { "probed": ${probed[cpu]},  "score": ${scores[cpu]},  "duration_ms": ${durations[cpu]},  "error": ${errors[cpu]} },
    "cuda": { "probed": ${probed[cuda]}, "score": ${scores[cuda]}, "duration_ms": ${durations[cuda]}, "error": ${errors[cuda]} },
    "sycl": { "probed": ${probed[sycl]}, "score": ${scores[sycl]}, "duration_ms": ${durations[sycl]}, "error": ${errors[sycl]} },
    "hip":  { "probed": ${probed[hip]},  "score": ${scores[hip]},  "duration_ms": ${durations[hip]},  "error": ${errors[hip]} }
  },
  "mcp_results": {
    "list_features": { "feature_count": ${mcp_fc_count:-null}, "duration_ms": ${mcp_fc_dur:-0}, "error": ${mcp_fc_err:-null} },
    "compute_vmaf":  { "score": ${mcp_cv_score:-null}, "duration_ms": ${mcp_cv_dur:-0}, "error": ${mcp_cv_err:-null} }
  }
}
JSONEOF

  echo "[smoke-probe] Written: ${output_file}" >&2
}

# A failed sub-check fails the probe. The record used to be the only place a
# failure showed, and nothing read it: --once exited 0 and the loop went on
# without a word.
report_failures() {
  local output_file="$1" backend failed=()

  for backend in cpu cuda sycl hip; do
    [ "${errors[${backend}]}" = "null" ] || failed+=("backend ${backend}")
  done
  [ "${mcp_fc_err:-null}" = "null" ] || failed+=("mcp list_extractors")
  [ "${mcp_cv_err:-null}" = "null" ] || failed+=("mcp vmaf_score")
  if [ "${#failed[@]}" -gt 0 ]; then
    local IFS=','
    echo "[smoke-probe] FAILED sub-checks: ${failed[*]} (see ${output_file})" >&2
    return 1
  fi
}

run_probe() {
  local output_file="${1}"
  local ts host_id

  ts="$(ts_now)"
  host_id="$(hostname):$(cat /proc/self/cgroup 2>/dev/null | grep -oP '(?<=docker-)[a-f0-9]{12}' | head -1 || echo 'unknown')"

  echo "[smoke-probe] Running probe at ${ts}…" >&2
  probe_backends
  probe_mcp
  write_record "${output_file}" "${ts}" "${host_id}"
  report_failures "${output_file}"
}

# ---------------------------------------------------------------------------
# Health: the newest record is recent and holds no error
# ---------------------------------------------------------------------------
_HEALTH_PY="$(
  cat <<'PY'
import json
import pathlib
import sys
import time

directory, max_age = pathlib.Path(sys.argv[1]), float(sys.argv[2])
records = sorted(directory.glob("probe-*.json"), key=lambda path: path.stat().st_mtime)
if not records:
    sys.exit(f"unhealthy: no probe record in {directory}")
latest = records[-1]
age = time.time() - latest.stat().st_mtime
if age > max_age:
    sys.exit(f"unhealthy: newest record {latest.name} is {age:.0f} s old (limit {max_age:.0f} s)")
try:
    record = json.loads(latest.read_text(encoding="utf-8"))
    groups = [record["backend_results"], record["mcp_results"]]
    failed = [name for group in groups for name, result in group.items() if result["error"]]
except (ValueError, KeyError, TypeError) as exc:
    sys.exit(f"unhealthy: {latest.name} is not a probe record ({exc})")
if failed:
    sys.exit(f"unhealthy: {latest.name} failed {', '.join(failed)}")
print(f"healthy: {latest.name}")
PY
)"

probe_health() {
  python3 -c "${_HEALTH_PY}" "${PROBE_OUTPUT_DIR}" "$((PROBE_INTERVAL * 2 + 300))"
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
  return 0
fi

ONCE=false
HEALTHY=false
OUTPUT_OVERRIDE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --once)
      ONCE=true
      shift
      ;;
    --healthy)
      HEALTHY=true
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

if "${HEALTHY}"; then
  probe_health
  exit
fi

if "${ONCE}"; then
  ts_tag="$(date +%Y%m%dT%H%M%S)"
  out="${OUTPUT_OVERRIDE:-${PROBE_OUTPUT_DIR}/probe-${ts_tag}.json}"
  if run_probe "${out}"; then
    exit 0
  fi
  exit 1
fi

# Continuous loop
echo "[smoke-probe-loop] Starting continuous probe loop (interval: ${PROBE_INTERVAL}s)" >&2

for ((cycle = 0; cycle < PROBE_MAX_CYCLES; cycle++)); do
  ts_tag="$(date +%Y%m%dT%H%M%S)"
  out="${PROBE_OUTPUT_DIR}/probe-${ts_tag}.json"
  run_probe "${out}" || echo "[smoke-probe-loop] WARNING: probe failed at ${ts_tag}" >&2
  echo "[smoke-probe-loop] Sleeping ${PROBE_INTERVAL}s until next probe…" >&2
  sleep "${PROBE_INTERVAL}"
done
