#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# Refuse Kubernetes E2E mutations unless they target the named local kind
# cluster through the dedicated kubeconfig supplied by the caller.

set -euo pipefail

CLUSTER_NAME="${KIND_CLUSTER_NAME:-vmafx-e2e}"
KUBECONFIG_PATH="${VMAFX_E2E_KUBECONFIG:-}"
EXPECTED_CONTEXT="kind-${CLUSTER_NAME}"

die() {
  printf '[assert-kind-context] ERROR: %s\n' "$*" >&2
  exit 1
}

command -v kubectl >/dev/null 2>&1 || die "required tool not found: kubectl"
[[ -n "${KUBECONFIG_PATH}" ]] ||
  die "VMAFX_E2E_KUBECONFIG must name a dedicated kubeconfig"
[[ "${KUBECONFIG_PATH}" = /* ]] ||
  die "VMAFX_E2E_KUBECONFIG must be an absolute path"
[[ "${KUBECONFIG_PATH}" != */.kube/config ]] ||
  die "the process-wide default kubeconfig is not a dedicated E2E file"
[[ ! -L "${KUBECONFIG_PATH}" ]] ||
  die "dedicated kubeconfig must not be a symbolic link"
[[ -s "${KUBECONFIG_PATH}" ]] ||
  die "dedicated kubeconfig is missing or empty: ${KUBECONFIG_PATH}"
[[ "${KUBECONFIG:-}" = "${KUBECONFIG_PATH}" ]] ||
  die "KUBECONFIG must equal VMAFX_E2E_KUBECONFIG"

current_context="$(
  kubectl --kubeconfig "${KUBECONFIG_PATH}" config current-context
)"
[[ "${current_context}" = "${EXPECTED_CONTEXT}" ]] ||
  die "context is ${current_context@Q}; expected ${EXPECTED_CONTEXT@Q}"

api_server="$(
  kubectl --kubeconfig "${KUBECONFIG_PATH}" config view --minify \
    -o jsonpath='{.clusters[0].cluster.server}'
)"
[[ "${api_server}" =~ ^https://127\.0\.0\.1:[0-9]+$ ]] ||
  die "API server is not a loopback kind endpoint: ${api_server@Q}"

printf '[assert-kind-context] verified context=%s server=%s\n' \
  "${current_context}" "${api_server}"
