#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# check-sycl-feature-routing.sh — verify vf_vmafx.c configures SYCL backend routing,
# feature spec passing to VMAFx engine, and zero-copy surface formats (ADR-2125, ADR-3360).
#
# Exit codes:
#   0  all assertions pass
#   1  at least one assertion failed or file missing

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="${HERE}/../src/vf_vmafx.c"

if [ ! -f "${TARGET}" ]; then
  echo "FAIL: ${TARGET} not found" >&2
  exit 1
fi

fail=0

check() {
  local label="$1"
  local pattern="$2"
  if grep -qE "${pattern}" "${TARGET}"; then
    echo "PASS: ${label}"
  else
    echo "FAIL: ${label} — pattern '${pattern}' not found in $(basename "${TARGET}")" >&2
    fail=1
  fi
}

check "SYCL backend slot defined" 'VMAFX_BACKEND_SYCL'
check "SYCL backend slot named sycl" '\{VMAFX_BACKEND_SYCL, "sycl"'
check "feature configuration delegates to context" 'vmafx_context_use_feature_spec\('
check "surface formats include NV12" 'AV_PIX_FMT_NV12'
check "surface formats include P010LE" 'AV_PIX_FMT_P010LE'
check "zero-copy DRM import wired for SYCL" 'VMAFX_DRM_IMPORT'

exit "${fail}"
