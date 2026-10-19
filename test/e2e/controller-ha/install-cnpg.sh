#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# test/e2e/controller-ha/install-cnpg.sh
#
# Install the CloudNativePG operator, the database prerequisite of the
# chart's PostgreSQL store (ADR-2350, Q-125), into the dedicated kind
# cluster: the release manifest is checked against its pinned SHA-256, and
# the operator image (which the operator also injects into every database
# pod) is rewritten from the manifest's tag to the pinned digest.
#
# Usage: VMAFX_E2E_KUBECONFIG=/abs/path ./test/e2e/controller-ha/install-cnpg.sh

set -euo pipefail

CNPG_VERSION="1.30.1"
CNPG_MANIFEST_SHA256="37237f145d8138256ea25ae830f87759255665ff08f8d552fdd8224a5ec032fb"
CNPG_IMAGE_DIGEST="sha256:923c267ec29636db3bee20f993d0ec4973fa22998e1adad37da79e4d32b5bc07"

REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
KUBECONFIG_PATH="${VMAFX_E2E_KUBECONFIG:-}"
[[ -n "${KUBECONFIG_PATH}" ]] || {
  printf 'VMAFX_E2E_KUBECONFIG must name the dedicated kind kubeconfig\n' >&2
  exit 1
}
export KUBECONFIG="${KUBECONFIG_PATH}"
"${REPO_ROOT}/test/e2e/assert-kind-context.sh"

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

curl --fail --location --show-error --silent \
  --connect-timeout 20 --max-time 300 \
  --retry 5 --retry-all-errors \
  --output "${work}/cnpg.yaml" \
  "https://github.com/cloudnative-pg/cloudnative-pg/releases/download/v${CNPG_VERSION}/cnpg-${CNPG_VERSION}.yaml"
printf '%s  %s\n' "${CNPG_MANIFEST_SHA256}" "${work}/cnpg.yaml" | sha256sum --check --strict

tagged="ghcr.io/cloudnative-pg/cloudnative-pg:${CNPG_VERSION}"
sed "s#${tagged}\$#${tagged}@${CNPG_IMAGE_DIGEST}#" "${work}/cnpg.yaml" >"${work}/cnpg-pinned.yaml"
# The manifest names the image twice: the operator container and
# OPERATOR_IMAGE_NAME, the instance manager of the database pods.
pinned="$(grep -c "${tagged}@${CNPG_IMAGE_DIGEST}\$" "${work}/cnpg-pinned.yaml")"
[[ "${pinned}" -eq 2 ]] || {
  printf 'expected 2 operator image references to pin, found %s\n' "${pinned}" >&2
  exit 1
}

kubectl apply --server-side --field-manager=vmafx-e2e -f "${work}/cnpg-pinned.yaml"
kubectl wait --for=condition=Established crd/clusters.postgresql.cnpg.io --timeout=60s
kubectl rollout status deployment/cnpg-controller-manager \
  --namespace cnpg-system --timeout=240s
