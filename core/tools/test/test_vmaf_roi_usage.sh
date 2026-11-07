#!/bin/sh
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2

# test_vmaf_roi_usage.sh — pin the usage text of the vmaf_roi sidecar
# (issue #3214 row C15-12; ADR-3402).
#
# print_usage() is a hand-written fprintf block, so nothing else guards it: a
# wording or flag change would reach users with no test noticing. The roi
# half is its own Meson test (split from the vpl half on review) so the
# golden check is a real PASS in every build instead of being folded into a
# skip when the SYCL-only vmaf_vpl binary is absent.
#
# The roi text has no argv[0] placeholder; Windows CRLF is stripped on both
# sides.
set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
ROI=./tools/vmaf_roi
WORK="${MESON_BUILD_ROOT:-.}/test_vmaf_roi_usage.scratch"
mkdir -p "${WORK}"

fail() {
  echo "test_vmaf_roi_usage: $1" >&2
  exit 1
}

# --help prints the golden usage on stdout, exits 0, writes nothing to
# stderr.
"$ROI" --help >"${WORK}/roi.out" 2>"${WORK}/roi.err" ||
  fail "vmaf_roi --help exited non-zero"
if [ -s "${WORK}/roi.err" ]; then
  fail "vmaf_roi --help wrote to stderr"
fi
sed -e 's/\r$//' "$SCRIPT_DIR/golden/vmaf_roi_help.txt" >"${WORK}/roi_golden.norm"
sed -e 's/\r$//' "${WORK}/roi.out" >"${WORK}/roi.norm"
if ! diff -u "${WORK}/roi_golden.norm" "${WORK}/roi.norm" >"${WORK}/roi.diff"; then
  head -20 "${WORK}/roi.diff" >&2
  fail "vmaf_roi usage text drifted from the golden transcript"
fi

rm -rf "${WORK}"
echo "PASS: $0"
