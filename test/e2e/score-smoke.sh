#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# Send a real file-backed score through the Helm Service and fail unless the
# response contains finite, internally consistent VMAF values.

set -euo pipefail

NAMESPACE="${VMAFX_E2E_NAMESPACE:-vmafx-e2e-test}"
SERVICE="${VMAFX_E2E_SERVICE:-vmafx}"
LOCAL_PORT="${VMAFX_E2E_LOCAL_PORT:-}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KUBECONFIG_PATH="${VMAFX_E2E_KUBECONFIG:-}"

for tool in curl kubectl python3; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    printf 'required tool not found: %s\n' "${tool}" >&2
    exit 1
  fi
done

if [[ -z "${LOCAL_PORT}" ]]; then
  LOCAL_PORT="$(
    python3 - <<'PYEOF'
import socket

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
    listener.bind(("127.0.0.1", 0))
    print(listener.getsockname()[1])
PYEOF
  )"
fi
if [[ ! "${LOCAL_PORT}" =~ ^[0-9]+$ ]] ||
  ((LOCAL_PORT < 1 || LOCAL_PORT > 65535)); then
  printf 'VMAFX_E2E_LOCAL_PORT must be an integer from 1 through 65535\n' >&2
  exit 1
fi

[[ -n "${KUBECONFIG_PATH}" ]] || {
  printf 'VMAFX_E2E_KUBECONFIG must name the dedicated kind kubeconfig\n' >&2
  exit 1
}
export KUBECONFIG="${KUBECONFIG_PATH}"
"${SCRIPT_DIR}/assert-kind-context.sh"

tmp_dir="$(mktemp -d)"
port_forward_pid=""
cleanup() {
  status=$?
  if [[ -n "${port_forward_pid}" ]]; then
    kill "${port_forward_pid}" >/dev/null 2>&1 || true
    wait "${port_forward_pid}" >/dev/null 2>&1 || true
  fi
  if [[ "${status}" -ne 0 ]]; then
    cat "${tmp_dir}/port-forward.log" >&2 2>/dev/null || true
    kubectl logs --namespace "${NAMESPACE}" deployment/vmafx \
      --tail=200 >&2 2>/dev/null || true
  fi
  rm -rf "${tmp_dir}"
  exit "${status}"
}
trap cleanup EXIT

kubectl port-forward --address 127.0.0.1 --namespace "${NAMESPACE}" \
  "service/${SERVICE}" "${LOCAL_PORT}:8080" \
  >"${tmp_dir}/port-forward.log" 2>&1 &
port_forward_pid=$!

ready=false
for _ in $(seq 1 60); do
  if ! kill -0 "${port_forward_pid}" 2>/dev/null; then
    printf 'kubectl port-forward exited before the server became ready\n' >&2
    exit 1
  fi
  if curl --fail --silent --show-error \
    "http://127.0.0.1:${LOCAL_PORT}/readyz" >/dev/null 2>&1; then
    ready=true
    break
  fi
  sleep 1
done

if [[ "${ready}" != "true" ]]; then
  printf 'vmafx-server did not become ready through the chart Service\n' >&2
  exit 1
fi

curl --fail-with-body --silent --show-error \
  --header 'Content-Type: application/json' \
  --data '{"reference":"/fixtures/ref.y4m","distorted":"/fixtures/dist.y4m"}' \
  "http://127.0.0.1:${LOCAL_PORT}/v1/score" \
  >"${tmp_dir}/score.json"

python3 - "${tmp_dir}/score.json" <<'PYEOF'
import json
import math
import pathlib
import sys

response_path = pathlib.Path(sys.argv[1])
response = json.loads(response_path.read_text(encoding="utf-8"))

score = response.get("score")
features = response.get("features")
if isinstance(score, bool) or not isinstance(score, (int, float)) or not math.isfinite(score):
    raise SystemExit(f"score is not finite: {score!r}")
if not isinstance(features, dict):
    raise SystemExit(f"features is not an object: {features!r}")
feature_vmaf = features.get("vmaf")
if (
    isinstance(feature_vmaf, bool)
    or not isinstance(feature_vmaf, (int, float))
    or not math.isfinite(feature_vmaf)
):
    raise SystemExit(f"features.vmaf is not finite: {feature_vmaf!r}")
if not math.isclose(score, feature_vmaf, rel_tol=0.0, abs_tol=1e-12):
    raise SystemExit(
        f"score {score!r} does not match pooled features.vmaf {feature_vmaf!r}"
    )

print(f"vmafx-server CPU score smoke passed: score={score:.6f}")
PYEOF
