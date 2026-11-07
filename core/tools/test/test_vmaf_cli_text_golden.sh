#!/bin/sh
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2

# test_vmaf_cli_text_golden.sh — pin the user-facing text of the `vmaf` CLI
# (issue #3214 rows C15-01, C15-02, C15-03, C15-07, C15-09, C15-10; ADR-3402).
#
# Help text, version format, option-error transcripts and the yuv geometry
# refusals are what a user actually reads; before this test any of them could
# change silently (the codegen unit tests cover emitter shaping on synthetic
# fixtures, not what the built binary serves, and dnn/test_cli.sh greps only
# three flag strings). Every case here diffs the real binary's output against
# the transcripts under test/golden/ — a text change fails here until the
# golden is consciously regenerated.
#
# Portability: the first "Usage: ..." line embeds argv[0], which differs
# across platforms and invocation paths, so it is normalised to
# "Usage: <app> [options]" on both the golden and the captured output; the
# same for the argv[0] prefix of the --precision refusal. Error transcripts
# are "<reason>", an empty line, then the full usage dump, so the usage head
# sits on line 3 there. Windows writes CRLF through a pipe (CRT text mode),
# so \r is stripped on both sides too.
set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
GOLDEN="$SCRIPT_DIR/golden/vmaf_help.txt"
BIN=./tools/vmaf
ALIAS=./tools/vmafx
WORK="${MESON_BUILD_ROOT:-.}/test_vmaf_cli_text_golden.scratch"
mkdir -p "${WORK}"

fail() {
  echo "test_vmaf_cli_text_golden: $1" >&2
  exit 1
}

# Strip CR and blank the argv[0] of a "Usage: ... [options]" first line
# (help/alias stdout).
norm_help() {
  sed -e 's/\r$//' \
      -e '1s/^Usage: .* \[options\]$/Usage: <app> [options]/' "$1"
}

# Error transcripts: keep the reason line, blank the argv[0] of the usage
# head on line 3.
norm_err() {
  sed -e 's/\r$//' \
      -e '3s/^Usage: .* \[options\]$/Usage: <app> [options]/' "$1"
}

# --help must print the golden transcript on stdout, exit 0, and write
# nothing to stderr. (C15-01)
"$BIN" --help >"${WORK}/help.out" 2>"${WORK}/help.err" ||
  fail "vmaf --help exited non-zero"
if [ -s "${WORK}/help.err" ]; then
  fail "vmaf --help wrote to stderr"
fi
sed -e 's/\r$//' "$GOLDEN" >"${WORK}/golden.norm"
norm_help "${WORK}/help.out" >"${WORK}/help.norm"
if ! diff -u "${WORK}/golden.norm" "${WORK}/help.norm" >"${WORK}/help.diff"; then
  head -30 "${WORK}/help.diff" >&2
  fail "vmaf --help drifted from the golden transcript"
fi

# The vmafx alias serves byte-identical help (modulo argv[0]). (C15-02)
"$ALIAS" --help >"${WORK}/alias.out" 2>"${WORK}/alias.err" ||
  fail "vmafx --help exited non-zero"
if [ -s "${WORK}/alias.err" ]; then
  fail "vmafx --help wrote to stderr"
fi
norm_help "${WORK}/alias.out" >"${WORK}/alias.norm"
if ! diff -u "${WORK}/golden.norm" "${WORK}/alias.norm" >"${WORK}/alias.diff"; then
  head -30 "${WORK}/alias.diff" >&2
  fail "vmafx alias help differs from the vmaf golden transcript"
fi

# --version is a format contract, not a value: v<major>.<minor>.<patch>
# optionally followed by the -<N>-g<sha> describe suffix. (C15-03)
VER_RE='^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9]+-g[0-9a-f]+)?$'
"$BIN" --version >"${WORK}/ver.out" 2>"${WORK}/ver.err" ||
  fail "vmaf --version exited non-zero"
if [ -s "${WORK}/ver.out" ]; then
  fail "vmaf --version wrote to stdout"
fi
sed -e 's/\r$//' "${WORK}/ver.err" >"${WORK}/ver.norm"
if ! grep -Eq "$VER_RE" "${WORK}/ver.norm"; then
  cat "${WORK}/ver.norm" >&2
  fail "vmaf --version does not match the version format"
fi

# The alias reports the same version in its own VMAFX form, pinning the
# ADR-0690 alias behaviour end to end. (C15-02)
"$ALIAS" --version >"${WORK}/aver.out" 2>"${WORK}/aver.err" ||
  fail "vmafx --version exited non-zero"
if [ -s "${WORK}/aver.out" ]; then
  fail "vmafx --version wrote to stdout"
fi
sed -e 's/\r$//' "${WORK}/aver.err" >"${WORK}/aver.norm"
if ! grep -Eq '^VMAFX v[0-9]+\.[0-9]+\.[0-9]+(-[0-9]+-g[0-9a-f]+)? \(auto-backend, precision=max\)$' \
  "${WORK}/aver.norm"; then
  cat "${WORK}/aver.norm" >&2
  fail "vmafx --version does not match the VMAFX version format"
fi

# Error transcripts: most refusals are <reason line>, an empty line, then the
# full usage dump — so the expected file is reason + the help golden, and a
# drift in either half is caught. The --precision refusal prints its single
# line and exits without the usage dump (tail = none). $4 = whether line 1
# embeds argv[0]; $5 = whether the usage dump follows. (C15-07, C15-09,
# C15-10)
err_case() {
  desc=$1
  want_rc=$2
  reason=$3
  argv0_prefix=$4
  usage_tail=$5
  shift 5
  sed -e 's/\r$//' "$GOLDEN" >"${WORK}/golden.norm"
  if [ "$usage_tail" = usage ]; then
    { printf '%s\n\n' "$reason"; cat "${WORK}/golden.norm"; } >"${WORK}/expected.err"
  else
    printf '%s\n' "$reason" >"${WORK}/expected.err"
  fi
  rc=0
  "$@" >"${WORK}/case.out" 2>"${WORK}/case.err" || rc=$?
  if [ "$rc" -eq 0 ]; then
    fail "$desc exited 0, expected $want_rc"
  fi
  if [ "$rc" -ne "$want_rc" ]; then
    fail "$desc exited $rc, expected $want_rc"
  fi
  if [ -s "${WORK}/case.out" ]; then
    fail "$desc wrote to stdout"
  fi
  if [ "$argv0_prefix" = argv0 ]; then
    sed -E -e 's/\r$//' \
        -e '1s|.*vmaf(\.exe)?: |<app>: |' \
        -e '3s/^Usage: .* \[options\]$/Usage: <app> [options]/' \
        "${WORK}/case.err" >"${WORK}/case.norm"
  else
    norm_err "${WORK}/case.err" >"${WORK}/case.norm"
  fi
  if ! diff -u "${WORK}/expected.err" "${WORK}/case.norm" >"${WORK}/case.diff"; then
    head -10 "${WORK}/case.diff" >&2
    fail "$desc transcript drifted"
  fi
}

err_case "unparsable --width" 1 \
  'Invalid argument "nope" for option -w/--width; should be an integer in [0, 2^32-1]' \
  plain usage "$BIN" --width nope

err_case "negative --width" 1 \
  'Invalid argument "-1" for option -w/--width; should be a non-negative integer' \
  plain usage "$BIN" --width -1

err_case "zero --width" 1 \
  '--width must be > 0' \
  plain usage "$BIN" --width 0

err_case "unparsable --precision" 1 \
  '<app>: --precision must be an integer 1..17, or one of: max, full, legacy (got: bogus)' \
  argv0 none "$BIN" --precision bogus

err_case "unparsable --model" 1 \
  'Problem parsing model, bad option string "unparseable model".' \
  plain usage "$BIN" --model 'unparseable model'

# The raw-yuv geometry refusals exit 2 with the declared-vs-actual sizes in
# the message; a 24x24 8-bit 420 frame is 864 bytes. (C15-09)
python3 - "$WORK" <<'PY'
import sys
from pathlib import Path
work = Path(sys.argv[1])
(work / "too_small.yuv").write_bytes(b"\x80" * 100)
(work / "not_multiple.yuv").write_bytes(b"\x80" * (864 * 2 + 100))
PY

raw_case() {
  desc=$1
  want_rc=$2
  reason=$3
  file=$4
  printf '%s\n' "$reason" >"${WORK}/expected.err"
  rc=0
  "$BIN" --reference "$file" --distorted "$file" \
      --width 24 --height 24 --pixel_format 420 --bitdepth 8 \
      >"${WORK}/case.out" 2>"${WORK}/case.err" || rc=$?
  if [ "$rc" -eq 0 ]; then
    fail "$desc exited 0, expected $want_rc"
  fi
  if [ "$rc" -ne "$want_rc" ]; then
    fail "$desc exited $rc, expected $want_rc"
  fi
  if [ -s "${WORK}/case.out" ]; then
    fail "$desc wrote to stdout"
  fi
  sed -e 's/\r$//' -e '1d' "${WORK}/case.err" >"${WORK}/case.extra"
  if [ -s "${WORK}/case.extra" ]; then
    fail "$desc printed more than the refusal line on stderr"
  fi
  sed -e 's/\r$//' "${WORK}/case.err" >"${WORK}/case.norm"
  if ! diff -u "${WORK}/expected.err" "${WORK}/case.norm" >"${WORK}/case.diff"; then
    head -6 "${WORK}/case.diff" >&2
    fail "$desc refusal text drifted"
  fi
}

raw_case "yuv too small for geometry" 2 \
  'yuv: file too small for declared geometry — need at least 864 bytes for one 24x24 8-bit yuv420p frame, got 100 bytes' \
  "${WORK}/too_small.yuv"

raw_case "yuv size not a frame multiple" 2 \
  'yuv: file size mismatch — expected a multiple of 864 bytes for 24x24 8-bit yuv420p, got 1828 bytes (hint: check --bitdepth and --pixel_format; 8-bit frames need 1 byte per sample)' \
  "${WORK}/not_multiple.yuv"

rm -rf "${WORK}"
echo "PASS: $0"
