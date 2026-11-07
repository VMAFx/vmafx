#!/bin/sh
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2

# test_vmaf_roi_vpl_usage.sh — pin the usage text of the vmaf_roi and
# vmaf_vpl sidecars (issue #3214 rows C15-12, C15-13; ADR-3402).
#
# Both print_usage() bodies are hand-written fprintf blocks, so nothing else
# guards them: a wording or flag change would reach users with no test
# noticing. vmaf_vpl ships only in Linux SYCL builds (VPL + VA-API), so when
# the binary is absent the vpl half skips (exit 77) rather than fails; its
# golden is verified by the Ubuntu SYCL lanes. The vpl golden embeds the
# VMAF_DEFAULT_MODEL_VERSION default on purpose — a default-model bump must
# consciously regenerate it.
#
# Portability: the vpl "Usage:" line embeds argv[0] and is normalised to
# "Usage: <app> ..."; Windows CRLF is stripped on both sides. The roi text
# has no argv[0] placeholder at all.
set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
ROI=./tools/vmaf_roi
VPL=./tools/vmaf_vpl
WORK="${MESON_BUILD_ROOT:-.}/test_vmaf_roi_vpl_usage.scratch"
mkdir -p "${WORK}"

fail() {
  echo "test_vmaf_roi_vpl_usage: $1" >&2
  exit 1
}

# --help prints the golden usage on stdout, exits 0, writes nothing to
# stderr. (C15-12)
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

# vmaf_vpl is built only for Linux SYCL (VPL + VA-API + Level Zero); skip
# elsewhere. (C15-13)
if [ ! -x "$VPL" ]; then
  echo "test_vmaf_roi_vpl_usage: vmaf_vpl not built (non-SYCL build); skipping the vpl half" >&2
  rm -rf "${WORK}"
  exit 77 # meson's "skipped"
fi

# --help prints the golden usage on stderr (argv[0] normalised), exits 0,
# writes nothing to stdout. (C15-13)
"$VPL" --help >"${WORK}/vpl.out" 2>"${WORK}/vpl.err" ||
  fail "vmaf_vpl --help exited non-zero"
if [ -s "${WORK}/vpl.out" ]; then
  fail "vmaf_vpl --help wrote to stdout"
fi
sed -e 's/\r$//' \
    -e '1s/^Usage: .* --ref <file> --dis <file> \[options\]$/Usage: <app> --ref <file> --dis <file> [options]/' \
    "${WORK}/vpl.err" >"${WORK}/vpl.norm"
sed -e 's/\r$//' "$SCRIPT_DIR/golden/vmaf_vpl_help.txt" >"${WORK}/vpl_golden.norm"
if ! diff -u "${WORK}/vpl_golden.norm" "${WORK}/vpl.norm" >"${WORK}/vpl.diff"; then
  head -20 "${WORK}/vpl.diff" >&2
  fail "vmaf_vpl usage text drifted from the golden transcript"
fi

# Without --ref/--dis the refusal is the specific first line followed by the
# same golden usage, exiting 1.
{ printf '%s\n' 'Error: --ref and --dis are required'; \
  sed -e 's/\r$//' "$SCRIPT_DIR/golden/vmaf_vpl_help.txt"; } \
  >"${WORK}/vpl_expected.err"
rc=0
"$VPL" >"${WORK}/vpl2.out" 2>"${WORK}/vpl2.err" || rc=$?
if [ "$rc" -eq 0 ]; then
  fail "vmaf_vpl without --ref/--dis exited 0"
fi
if [ "$rc" -ne 1 ]; then
  fail "vmaf_vpl without --ref/--dis exited $rc, expected 1"
fi
sed -e 's/\r$//' \
    -e '2s/^Usage: .* --ref <file> --dis <file> \[options\]$/Usage: <app> --ref <file> --dis <file> [options]/' \
    "${WORK}/vpl2.err" >"${WORK}/vpl2.norm"
if ! diff -u "${WORK}/vpl_expected.err" "${WORK}/vpl2.norm" >"${WORK}/vpl2.diff"; then
  head -10 "${WORK}/vpl2.diff" >&2
  fail "vmaf_vpl missing-required transcript drifted"
fi

rm -rf "${WORK}"
echo "PASS: $0"
