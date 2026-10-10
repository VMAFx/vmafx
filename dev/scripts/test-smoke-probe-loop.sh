#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris

set -euo pipefail
IFS=$'\n\t'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TEST_ROOT="$(mktemp -d)"

cleanup() {
  rm -rf "${TEST_ROOT}"
}
trap cleanup EXIT INT TERM

mkdir -p "${TEST_ROOT}/bin" "${TEST_ROOT}/testdata" "${TEST_ROOT}/model"
touch "${TEST_ROOT}/testdata/ref_576x324_48f.yuv"
touch "${TEST_ROOT}/testdata/dis_576x324_48f.yuv"
touch "${TEST_ROOT}/model/vmaf_v0.6.1.json"

export VMAFX_SMOKE_VMAF_CALLS="${TEST_ROOT}/vmaf-calls.log"
export VMAFX_SMOKE_MCP_CALLS="${TEST_ROOT}/mcp-calls.log"

cat >"${TEST_ROOT}/bin/vmaf" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail

backend=""
output=""
json=false
pixel_format=""
bitdepth=""
original=("$@")
while [[ $# -gt 0 ]]; do
  case "$1" in
    --backend)
      backend="$2"
      shift 2
      ;;
    --backend=*)
      backend="${1#--backend=}"
      shift
      ;;
    --output)
      output="$2"
      shift 2
      ;;
    --json)
      json=true
      shift
      ;;
    --cuda | --sycl | --hip | --no_prediction_flags | --no_prediction)
      printf 'obsolete or score-suppressing flag: %s\n' "$1" >&2
      exit 91
      ;;
    --pixel_format)
      pixel_format="$2"
      shift 2
      ;;
    --bitdepth)
      bitdepth="$2"
      shift 2
      ;;
    --reference | --distorted | --width | --height | --model)
      shift 2
      ;;
    *)
      shift
      ;;
  esac
done

printf '%q ' "${original[@]}" >>"${VMAFX_SMOKE_VMAF_CALLS}"
printf '\n' >>"${VMAFX_SMOKE_VMAF_CALLS}"

[[ -n "${backend}" ]]
[[ -n "${output}" && "${output}" != /dev/null ]]
[[ "${pixel_format}" == 420 ]]
[[ "${bitdepth}" == 8 ]]
"${json}"

if [[ "${VMAFX_SMOKE_FAIL_BACKEND:-}" == "${backend}" ]]; then
  printf 'forced failure: "quoted" \\ path\twith-tab\n' >&2
  exit 93
fi

case "${backend}" in
  cpu) score=90.1 ;;
  cuda) score=91.2 ;;
  sycl) score=92.3 ;;
  hip) score=93.4 ;;
  *) exit 92 ;;
esac

reported_backend="${backend}"
if [[ "${VMAFX_SMOKE_MISMATCH_BACKEND:-}" == "${backend}" ]]; then
  reported_backend=cpu
fi
printf '{"pooled_metrics":{"vmaf":{"mean":%s}},"backend_used":"%s"}\n' \
  "${score}" "${reported_backend}" >"${output}"
STUB

cat >"${TEST_ROOT}/bin/vmafx-mcp" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail

request=""
while IFS= read -r line; do
  request+="${line}"$'\n'
  if [[ "${line}" == *'"id":2'* ]]; then
    break
  fi
done

# Respond while the client still has stdin open. The production Go SDK also
# answers asynchronously; a client that closes stdin immediately races EOF
# against the response and receives nothing.
grep -q '"id":2' <<<"${request}"
printf '%s---\n' "${request}" >>"${VMAFX_SMOKE_MCP_CALLS}"
printf 'VMAF_MCP_ALLOW=%s\n' "${VMAF_MCP_ALLOW:-}" >>"${VMAFX_SMOKE_MCP_CALLS}"

# The production server refuses a path outside its allowlist
# (pkg/libvmaf/paths.go). The stub has no repository root, so only the roots in
# VMAF_MCP_ALLOW count, as in the container.
ref_allowed() {
  local ref root
  ref="$(sed -n 's/.*"ref":"\([^"]*\)".*/\1/p' <<<"${request}" | head -1)"
  local IFS=':'
  # VMAFX_SMOKE_MCP_NO_ALLOW: a server started without the declaration.
  [[ -z "${VMAFX_SMOKE_MCP_NO_ALLOW:-}" ]] || VMAF_MCP_ALLOW=""
  for root in ${VMAF_MCP_ALLOW:-}; do
    [[ -n "${root}" && "${ref}" == "${root}"/* ]] && return 0
  done
  printf '{"jsonrpc":"2.0","id":2,"result":{"content":[{"type":"text","text":"ref: path %s is not under an allowlisted root; set VMAF_MCP_ALLOW to extend"}],"isError":true}}\n' "${ref}"
  return 1
}

printf '%s\n' '{"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05","capabilities":{},"serverInfo":{"name":"stub","version":"1"}}}'
if grep -q '"name":"list_extractors"' <<<"${request}"; then
  printf '%s\n' '{"jsonrpc":"2.0","id":2,"result":{"content":[{"type":"text","text":"{\n  \"extractors\": [\n    {\"name\": \"vmaf\"},\n    {\"name\": \"psnr\"}\n  ]\n}"}]}}'
elif grep -q '"name":"vmaf_score"' <<<"${request}"; then
  if ref_allowed; then
    printf '%s\n' '{"jsonrpc":"2.0","id":2,"result":{"content":[{"type":"text","text":"{\n  \"pooled_metrics\": {\"vmaf\": {\"mean\": 88.8}},\n  \"backend_used\": \"cpu\"\n}"}]}}'
  fi
else
  printf '%s\n' '{"jsonrpc":"2.0","id":2,"result":{"isError":true,"content":[{"type":"text","text":"wrong tool"}]}}'
fi
STUB

cat >"${TEST_ROOT}/bin/vmaf-mcp-server" <<'STUB'
#!/usr/bin/env bash
printf 'retired vmaf-mcp-server invoked\n' >&2
exit 99
STUB

chmod +x "${TEST_ROOT}/bin/vmaf" "${TEST_ROOT}/bin/vmafx-mcp" \
  "${TEST_ROOT}/bin/vmaf-mcp-server"

export PATH="${TEST_ROOT}/bin:${PATH}"
export VMAF_TESTDATA_PATH="${TEST_ROOT}/testdata"
export VMAF_MODEL_PATH="${TEST_ROOT}/model"

# The defensive helper-failure record used to contain literal backslashes
# before its quotes, producing an invalid JSON value (\"probe failed\"). Source
# the production helper and prove its exact three-field record embeds cleanly.
fallback_record="$(
  bash -c 'source "$1"; probe_failed_record' _ \
    "${REPO_ROOT}/dev/scripts/smoke-probe-loop.sh"
)"
IFS=$'\x1f' read -r fallback_score fallback_duration fallback_error <<<"${fallback_record}"
python3 - "${fallback_score}" "${fallback_duration}" "${fallback_error}" <<'PY'
import json
import sys

score, duration, error = sys.argv[1:]
assert score == "null", score
assert duration == "0", duration
assert json.loads(error) == "probe failed", error
assert json.loads('{"error":' + error + "}") == {"error": "probe failed"}
PY

number_records="$(
  bash -c '
    source "$1"
    for value in 0 -0 94.323 1e-06 .5 1. 01 nan; do
      printf "%s=%s\n" "$value" "$(json_num "$value")"
    done
  ' _ "${REPO_ROOT}/dev/scripts/smoke-probe-loop.sh"
)"
python3 - "${number_records}" <<'PY'
import sys

records = dict(line.split("=", 1) for line in sys.argv[1].splitlines())
assert records == {
    "0": "0",
    "-0": "-0",
    "94.323": "94.323",
    "1e-06": "1e-06",
    ".5": "null",
    "1.": "null",
    "01": "null",
    "nan": "null",
}, records
PY

output="${TEST_ROOT}/probe.json"
VMAF_MCP_ALLOW=/caller/root \
  bash "${REPO_ROOT}/dev/scripts/smoke-probe-loop.sh" --once --output "${output}"

python3 - "${output}" "${VMAFX_SMOKE_VMAF_CALLS}" "${VMAFX_SMOKE_MCP_CALLS}" <<'PY'
import json
import math
import pathlib
import sys

probe_path, vmaf_calls_path, mcp_calls_path = map(pathlib.Path, sys.argv[1:])
probe = json.loads(probe_path.read_text(encoding="utf-8"))

expected = {"cpu": 90.1, "cuda": 91.2, "sycl": 92.3, "hip": 93.4}
for backend, score in expected.items():
    result = probe["backend_results"][backend]
    assert result["error"] is None, (backend, result)
    assert math.isclose(result["score"], score), (backend, result)

assert probe["mcp_results"]["list_features"]["error"] is None
assert probe["mcp_results"]["list_features"]["feature_count"] == 2
assert probe["mcp_results"]["compute_vmaf"]["error"] is None
assert math.isclose(probe["mcp_results"]["compute_vmaf"]["score"], 88.8)

vmaf_calls = vmaf_calls_path.read_text(encoding="utf-8")
for backend in expected:
    assert f"--backend {backend}" in vmaf_calls, vmaf_calls
assert "--json" in vmaf_calls
assert "--output /dev/null" not in vmaf_calls
assert "--no_prediction" not in vmaf_calls

mcp_calls = mcp_calls_path.read_text(encoding="utf-8")
assert mcp_calls.count('"method":"initialize"') == 2, mcp_calls
assert '"name":"list_extractors"' in mcp_calls, mcp_calls
assert '"name":"vmaf_score"' in mcp_calls, mcp_calls
assert '"name":"list_features"' not in mcp_calls, mcp_calls
assert '"name":"compute_vmaf"' not in mcp_calls, mcp_calls
assert '"pixfmt":"420"' in mcp_calls, mcp_calls
assert '"bitdepth":8' in mcp_calls, mcp_calls
assert '"backend":"cpu"' in mcp_calls, mcp_calls
assert str(probe_path.parent / "testdata" / "ref_576x324_48f.yuv") in mcp_calls
assert str(probe_path.parent / "testdata" / "dis_576x324_48f.yuv") in mcp_calls
# The probe declares its test-data directory, and nothing wider, to the
# server it starts; a root the caller set is kept.
allows = {line for line in mcp_calls.splitlines() if line.startswith("VMAF_MCP_ALLOW=")}
assert allows == {f"VMAF_MCP_ALLOW={probe_path.parent / 'testdata'}:/caller/root"}, allows
PY

# A path outside the allowlist stays refused: a server that did not get the
# declaration refuses the golden pair, as the container's did. The refusal
# reaches the record in the server's own words, and the probe exits 1.
refused_output="${TEST_ROOT}/probe-refused.json"
if VMAFX_SMOKE_MCP_NO_ALLOW=1 \
  bash "${REPO_ROOT}/dev/scripts/smoke-probe-loop.sh" --once --output "${refused_output}" \
  2>"${TEST_ROOT}/refused.log"; then
  echo "FAIL: a refused vmaf_score left the probe exit status at 0" >&2
  exit 1
fi
grep -q 'FAILED sub-checks: mcp vmaf_score' "${TEST_ROOT}/refused.log"
python3 - "${refused_output}" <<'PY'
import json
import pathlib
import sys

probe = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
refusal = probe["mcp_results"]["compute_vmaf"]
assert refusal["score"] is None, refusal
assert "is not under an allowlisted root" in refusal["error"], refusal
assert "testdata/ref_576x324_48f.yuv" in refusal["error"], refusal
assert probe["mcp_results"]["list_features"]["error"] is None
PY

# A backend failure used to shift the leading empty score out of a tab-split
# record and could inject quotes/control bytes directly into the enclosing
# JSON. The whole probe must remain parseable and preserve the diagnostic.
export VMAFX_SMOKE_FAIL_BACKEND=hip
error_output="${TEST_ROOT}/probe-error.json"
if bash "${REPO_ROOT}/dev/scripts/smoke-probe-loop.sh" --once --output "${error_output}" \
  2>"${TEST_ROOT}/error.log"; then
  echo "FAIL: a failed backend left the probe exit status at 0" >&2
  exit 1
fi
grep -q 'FAILED sub-checks: backend hip' "${TEST_ROOT}/error.log"
python3 - "${error_output}" <<'PY'
import json
import pathlib
import sys

probe = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
failure = probe["backend_results"]["hip"]
assert failure["score"] is None, failure
assert isinstance(failure["duration_ms"], int), failure
assert '"quoted"' in failure["error"], failure
assert "\\ path\twith-tab" in failure["error"], failure
PY

# A host declares the backends it has. A backend left out of PROBE_BACKENDS is
# not run, so its missing device cannot fail the probe; its entry says so.
: >"${VMAFX_SMOKE_VMAF_CALLS}"
declared_output="${TEST_ROOT}/probe-declared.json"
PROBE_BACKENDS="cpu,cuda sycl" \
  bash "${REPO_ROOT}/dev/scripts/smoke-probe-loop.sh" --once --output "${declared_output}"
python3 - "${declared_output}" "${VMAFX_SMOKE_VMAF_CALLS}" <<'PY'
import json
import pathlib
import sys

probe = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
results = probe["backend_results"]
assert results["hip"] == {"probed": False, "score": None, "duration_ms": 0, "error": None}
assert all(results[name]["probed"] and results[name]["error"] is None for name in ("cpu", "cuda", "sycl"))
assert "--backend hip" not in pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
PY
status=0
PROBE_BACKENDS="cpu vulkan" bash "${REPO_ROOT}/dev/scripts/smoke-probe-loop.sh" --once \
  --output "${TEST_ROOT}/probe-unknown.json" 2>"${TEST_ROOT}/unknown.log" || status=$?
[[ "${status}" -eq 2 ]]
grep -q 'unknown backend: vulkan' "${TEST_ROOT}/unknown.log"
unset VMAFX_SMOKE_FAIL_BACKEND

# A successful CLI exit is not enough: exclusive-backend smoke evidence is
# valid only when the JSON receipt confirms the requested backend actually ran.
export VMAFX_SMOKE_MISMATCH_BACKEND=sycl
mismatch_output="${TEST_ROOT}/probe-mismatch.json"
if bash "${REPO_ROOT}/dev/scripts/smoke-probe-loop.sh" --once --output "${mismatch_output}" \
  2>/dev/null; then
  echo "FAIL: a backend receipt mismatch left the probe exit status at 0" >&2
  exit 1
fi
python3 - "${mismatch_output}" <<'PY'
import json
import pathlib
import sys

probe = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
mismatch = probe["backend_results"]["sycl"]
assert mismatch["score"] is None, mismatch
assert "requested backend 'sycl'" in mismatch["error"], mismatch
assert "output reports 'cpu'" in mismatch["error"], mismatch
PY
unset VMAFX_SMOKE_MISMATCH_BACKEND

# --healthy reads the newest record: healthy only while it is recent and holds
# no error. An empty directory, a failed record and a stale record are not.
health() {
  PROBE_OUTPUT_DIR="$1" PROBE_INTERVAL_SECONDS=900 \
    bash "${REPO_ROOT}/dev/scripts/smoke-probe-loop.sh" --healthy 2>&1
}
records="${TEST_ROOT}/records"
mkdir -p "${records}"
if message="$(health "${records}")"; then
  echo "FAIL: no record counted as healthy" >&2
  exit 1
fi
grep -q 'no probe record' <<<"${message}"
cp "${output}" "${records}/probe-20260101T000000.json"
health "${records}" | grep -q '^healthy: probe-20260101T000000.json$'
cp "${error_output}" "${records}/probe-20260101T000100.json"
if message="$(health "${records}")"; then
  echo "FAIL: a record with a failed backend counted as healthy" >&2
  exit 1
fi
grep -q 'failed hip' <<<"${message}"
rm "${records}/probe-20260101T000100.json"
touch -d '2 hours ago' "${records}/probe-20260101T000000.json"
if message="$(health "${records}")"; then
  echo "FAIL: a two-hour-old record counted as healthy" >&2
  exit 1
fi
grep -q 's old' <<<"${message}"
