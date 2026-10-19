#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# test/e2e/controller-ha/run-driver.sh
#
# Run the controller failover scenario (driver.yaml) in the release's
# namespace, wait for its Job to complete or fail, print the driver's log,
# and pass only on a summary line with "ok":true.
#
# Usage: VMAFX_E2E_KUBECONFIG=/abs/path ./test/e2e/controller-ha/run-driver.sh [NAMESPACE]

set -euo pipefail

NAMESPACE="${1:-vmafx-ha}"
TIMEOUT_SECONDS=900
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../../.." && pwd)"
KUBECONFIG_PATH="${VMAFX_E2E_KUBECONFIG:-}"
[[ -n "${KUBECONFIG_PATH}" ]] || {
  printf 'VMAFX_E2E_KUBECONFIG must name the dedicated kind kubeconfig\n' >&2
  exit 1
}
export KUBECONFIG="${KUBECONFIG_PATH}"
"${REPO_ROOT}/test/e2e/assert-kind-context.sh"

# A rerun starts a fresh scenario; the driver reads only the jobs it submits.
kubectl delete job vmafx-e2e-driver --namespace "${NAMESPACE}" \
  --ignore-not-found --cascade=foreground --wait=true
kubectl apply --server-side --field-manager=vmafx-e2e \
  --namespace "${NAMESPACE}" -f "${HERE}/driver.yaml"

# Poll the Job's conditions: `kubectl wait` takes one condition, and
# waiting for Complete alone would sit out the whole timeout on a failure.
state=""
for _ in $(seq 1 $((TIMEOUT_SECONDS / 5))); do
  state="$(kubectl get job vmafx-e2e-driver --namespace "${NAMESPACE}" \
    -o jsonpath='{range .status.conditions[?(@.status=="True")]}{.type}{" "}{end}')"
  case " ${state} " in
    *" Complete "* | *" Failed "*) break ;;
  esac
  sleep 5
done

log="$(kubectl logs --namespace "${NAMESPACE}" job/vmafx-e2e-driver)"
printf '%s\n' "${log}"
summary="$(printf '%s\n' "${log}" | grep '^{' | tail -n 1)"
case " ${state} " in
  *" Complete "*) ;;
  *)
    printf 'driver Job did not complete (conditions: %s)\n' "${state:-none}" >&2
    exit 1
    ;;
esac
python3 -c 'import json, sys; s = json.loads(sys.argv[1]); sys.exit(0 if s.get("ok") is True else 1)' \
  "${summary}" || {
  printf 'driver summary is not ok: %s\n' "${summary}" >&2
  exit 1
}
