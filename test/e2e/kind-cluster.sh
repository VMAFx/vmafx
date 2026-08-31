#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# test/e2e/kind-cluster.sh
#
# Spin up a CPU-only kind cluster for the VMAFx runtime contract.
#
# The chart test selects gpu.vendor=cpu, so device plugins and cert-manager are
# intentionally absent. Neither is a prerequisite for the current chart or
# operator configuration, and unrelated downloads must not decide this lane.
#
# Prerequisites: Docker, kind >= 0.23, kubectl.
#
# Usage: ./test/e2e/kind-cluster.sh [--cluster-name NAME] [--teardown]
#
# Environment variables:
#   KIND_CLUSTER_NAME   override cluster name (default: vmafx-e2e)
#   VMAFX_E2E_KUBECONFIG absolute path to the dedicated test kubeconfig (required)
#   TEARDOWN            set to "1" to delete the cluster instead of creating it
#
# ADR-0783: k8s e2e integration test harness design.

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
CLUSTER_NAME="${KIND_CLUSTER_NAME:-vmafx-e2e}"
TEARDOWN="${TEARDOWN:-}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ASSERT_CONTEXT="${REPO_ROOT}/test/e2e/assert-kind-context.sh"
KUBECONFIG_PATH="${VMAFX_E2E_KUBECONFIG:-}"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
log() { printf '\033[1;34m[kind-cluster]\033[0m %s\n' "$*" >&2; }
die() {
  printf '\033[1;31m[kind-cluster] ERROR:\033[0m %s\n' "$*" >&2
  exit 1
}

require() {
  local cmd="$1"
  command -v "$cmd" >/dev/null 2>&1 || die "Required tool not found: $cmd"
}

require_isolated_kubeconfig_path() {
  [[ -n "${KUBECONFIG_PATH}" ]] ||
    die "VMAFX_E2E_KUBECONFIG must name a dedicated kubeconfig"
  [[ "${KUBECONFIG_PATH}" = /* ]] ||
    die "VMAFX_E2E_KUBECONFIG must be an absolute path"
  [[ "${KUBECONFIG_PATH}" != */.kube/config ]] ||
    die "the process-wide default kubeconfig is not a dedicated E2E file"
  [[ ! -L "${KUBECONFIG_PATH}" ]] ||
    die "dedicated kubeconfig must not be a symbolic link"
  if [[ -n "${KUBECONFIG:-}" && "${KUBECONFIG}" != "${KUBECONFIG_PATH}" ]]; then
    die "KUBECONFIG must equal VMAFX_E2E_KUBECONFIG"
  fi
  export KUBECONFIG="${KUBECONFIG_PATH}"
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
  case "$1" in
    --cluster-name)
      CLUSTER_NAME="$2"
      shift 2
      ;;
    --teardown)
      TEARDOWN=1
      shift
      ;;
    *) die "Unknown argument: $1" ;;
  esac
done
export KIND_CLUSTER_NAME="${CLUSTER_NAME}"

# ---------------------------------------------------------------------------
# Teardown path
# ---------------------------------------------------------------------------
if [ "${TEARDOWN}" = "1" ]; then
  require kind
  require kubectl
  require_isolated_kubeconfig_path
  if kind get clusters 2>/dev/null | grep -qx "${CLUSTER_NAME}"; then
    "${ASSERT_CONTEXT}"
    log "Deleting kind cluster: ${CLUSTER_NAME}"
    kind delete cluster --name "${CLUSTER_NAME}" \
      --kubeconfig "${KUBECONFIG_PATH}"
  else
    log "Kind cluster '${CLUSTER_NAME}' does not exist; nothing to delete."
  fi
  exit 0
fi

# ---------------------------------------------------------------------------
# Prerequisite checks
# ---------------------------------------------------------------------------
for tool in docker kind kubectl; do
  require "$tool"
done
require_isolated_kubeconfig_path

# ---------------------------------------------------------------------------
# Create (or reuse) cluster
# ---------------------------------------------------------------------------
if kind get clusters 2>/dev/null | grep -qx "${CLUSTER_NAME}"; then
  log "Cluster '${CLUSTER_NAME}' already exists — reusing."
else
  # Cluster creation is a local Docker operation, not a Kubernetes API
  # mutation. Refuse any pre-existing kubeconfig unless it already proves the
  # exact local kind identity; an absent dedicated file is created by kind.
  if [[ -e "${KUBECONFIG_PATH}" ]]; then
    "${ASSERT_CONTEXT}"
  fi
  log "Creating kind cluster '${CLUSTER_NAME}'..."
  kind create cluster --name "${CLUSTER_NAME}" \
    --kubeconfig "${KUBECONFIG_PATH}" --wait 120s
fi

# Export and verify the dedicated kubeconfig before any Kubernetes mutation.
kind export kubeconfig --name "${CLUSTER_NAME}" \
  --kubeconfig "${KUBECONFIG_PATH}"
"${ASSERT_CONTEXT}"
log "Dedicated kubeconfig: ${KUBECONFIG_PATH}"

# ---------------------------------------------------------------------------
# Install VMAFx CRDs without creating a chart workload. The kuttl case owns
# the single Helm release so a missing runtime image cannot be hidden behind
# the old best-effort CRD fallback.
# ---------------------------------------------------------------------------
log "Installing VMAFx CRDs..."
"${ASSERT_CONTEXT}"
kubectl --kubeconfig "${KUBECONFIG_PATH}" apply --server-side \
  -f "${REPO_ROOT}/deploy/helm/vmafx/crds/"
kubectl --kubeconfig "${KUBECONFIG_PATH}" wait --for=condition=Established \
  crd/vmafxjobs.vmafx.dev \
  crd/vmafxnodes.vmafx.dev \
  crd/vmafxmodeltrainings.vmafx.dev \
  --timeout=60s

# ---------------------------------------------------------------------------
# Pre-load VMAFx images into kind node (avoids pull-backoff in air-gapped CI)
# ---------------------------------------------------------------------------
VMAFX_IMAGES=(
  "ghcr.io/vmafx/vmafx-operator:e2e-test"
  "ghcr.io/vmafx/vmafx-node:e2e-test"
  "ghcr.io/vmafx/vmafx-server:e2e-test"
)
for img in "${VMAFX_IMAGES[@]}"; do
  if docker image inspect "${img}" >/dev/null 2>&1; then
    log "Pre-loading image into kind: ${img}"
    kind load docker-image "${img}" --name "${CLUSTER_NAME}"
  else
    log "Image not present locally, skipping pre-load: ${img}"
  fi
done

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
log "Cluster '${CLUSTER_NAME}' is ready."
log "Run the e2e tests with:"
log "  kubectl kuttl test --config test/e2e/kuttl-tests/kuttl-test.yaml"
