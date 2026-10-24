#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# check-sycl-feature-routing.sh — patch-text contract for libvmaf_sycl feature
# routing (ADR-3360).
#
# Asserts that patch 0005:
#   1. resolves feature= names through vmaf_feature_backend_twin;
#   2. replaces the vmaf_use_feature() call of parse_features with use_feature();
#   3. restricts QSV zero-copy to NV12 / P010 surfaces.
#
# What the filter does on a failed VA import is not checked here: that is the
# subject of VMAFx/vmafx#2110 (retry, then fail naming the frame) and its own
# check there.
#
# No FFmpeg build or GPU is needed; the patch text is inspected directly.
#
# Exit codes:
#   0  all assertions pass
#   1  at least one assertion failed
#   77 patch file not found (skip)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH="${HERE}/../0005-libvmaf-add-libvmaf-sycl-filter.patch"

if [ ! -f "${PATCH}" ]; then
  echo "SKIP: ${PATCH} not found" >&2
  exit 77
fi

fail=0

check() {
  local label="$1"
  local pattern="$2"
  if grep -qE "${pattern}" "${PATCH}"; then
    echo "PASS: ${label}"
  else
    echo "FAIL: ${label} — pattern '${pattern}' not found in $(basename "${PATCH}")" >&2
    fail=1
  fi
}

check "twin lookup used" 'vmaf_feature_backend_twin\('
check "parse_features calls use_feature()" 'use_feature\(ctx, s, feature_name, feature_opts_dict\)'
check "upstream vmaf_use_feature call site replaced" '^-.*vmaf_use_feature\(s->vmaf, feature_name, feature_opts_dict\)'
check "QSV guard references NV12" 'AV_PIX_FMT_NV12'
check "QSV guard references P010" 'AV_PIX_FMT_P010'
check "QSV guard message" 'supports NV12 and P010 surfaces only'
check "zero-copy no-twin error" 'cannot run on zero-copy input'
check "host-upload CPU fallback warning" 'computing it on the CPU'

exit "${fail}"
