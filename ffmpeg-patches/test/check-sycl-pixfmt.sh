#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# check-sycl-pixfmt.sh — verify vf_vmafx.c exposes the expected pixel formats
# for the vmafx filter (software inputs, QSV, and DRM PRIME) and contains
# the software frame handling path (ADR-2125, issue #2435).
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

check "vmafx_pix_fmts includes AV_PIX_FMT_YUV420P" "AV_PIX_FMT_YUV420P,"
check "vmafx_pix_fmts includes AV_PIX_FMT_QSV" "AV_PIX_FMT_QSV,"
check "vmafx_pix_fmts includes AV_PIX_FMT_DRM_PRIME" "AV_PIX_FMT_DRM_PRIME,"
check "software frame conversion and layout handling present" "to_vmafx_frame|layouts|sw_format"

exit "${fail}"
