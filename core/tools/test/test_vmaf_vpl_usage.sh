#!/bin/sh
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2

# test_vmaf_vpl_usage.sh — pin the usage text of the vmaf_vpl sidecar
# (issue #3214 row C15-13; ADR-3402).
#
# print_usage() is a hand-written fprintf block, so nothing else guards it: a
# wording or flag change would reach users with no test noticing. vmaf_vpl
# ships only in Linux SYCL builds (VPL + VA-API + Level Zero), so outside
# them this test skips (exit 77) rather than fails; the golden is verified by
# the Ubuntu SYCL lanes. The golden embeds the VMAF_DEFAULT_MODEL_VERSION
# default on purpose — a default-model bump must consciously regenerate it.
#
# Portability: the "Usage:" line embeds argv[0] and is normalised to
# "Usage: <app> ..."; Windows CRLF is stripped on both sides.
set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
VPL=./tools/vmaf_vpl
WORK="${MESON_BUILD_ROOT:-.}/test_vmaf_vpl_usage.scratch"
mkdir -p "${WORK}"

fail() {
  echo "test_vmaf_vpl_usage: $1" >&2
  exit 1
}

# vmaf_vpl is built only for Linux SYCL (VPL + VA-API + Level Zero); skip
# elsewhere.
if [ ! -x "$VPL" ]; then
  echo "test_vmaf_vpl_usage: vmaf_vpl not built (non-SYCL build); skipping" >&2
  rm -rf "${WORK}"
  exit 77 # meson's "skipped"
fi

# --help prints the golden usage on stderr (argv[0] normalised), exits 0,
# writes nothing to stdout.
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
