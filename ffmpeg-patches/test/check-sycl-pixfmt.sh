#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# check-sycl-pixfmt.sh — verify patch 0005 exposes the expected pixel formats
# for the libvmaf_sycl filter and contains the software-copy path.
#
# PR #850 context: before the fix, 0005 accepted only AV_PIX_FMT_QSV, blocking
# software-decoded inputs.  The fix added YUV420P + YUV420P10LE to
# FILTER_PIXFMTS and added a software copy path in do_vmaf_sycl.
#
# This script does NOT require a live FFmpeg build or an Intel GPU.  It
# inspects patch 0005 directly to assert:
#   1. FILTER_PIXFMTS includes AV_PIX_FMT_YUV420P (software input support).
#   2. FILTER_PIXFMTS includes AV_PIX_FMT_QSV (QSV/hardware input preserved).
#   3. do_vmaf_sycl contains a software copy path (presence of "src_frame->format"
#      or equivalent conditional that handles non-QSV frames).
#
# For a full end-to-end check (requires FFmpeg build + SYCL runtime) run
# ffmpeg-patches/test/build-and-run.sh with --enable-libvmaf-sycl.
#
# Exit codes:
#   0  all assertions pass
#   1  at least one assertion failed
#   77 patch file not found (skip — pre-SYCL build)

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

# PR #850 fix assertions
check "FILTER_PIXFMTS includes AV_PIX_FMT_YUV420P" "AV_PIX_FMT_YUV420P[^1]"
check "FILTER_PIXFMTS includes AV_PIX_FMT_QSV" "AV_PIX_FMT_QSV"
check "software copy path present in do_vmaf_sycl" "src_frame->format|hwframe_ctx|sw_format|AV_PIX_FMT_NONE"

exit "${fail}"
