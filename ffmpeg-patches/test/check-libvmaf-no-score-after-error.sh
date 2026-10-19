#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# check-libvmaf-no-score-after-error.sh — after a mid-run error the libvmaf and
# libvmaf_cuda filters stop with one error naming the frame, exit non-zero,
# and print no pooled score and write no log (patch 0021, ADR-1768). Upstream
# FFmpeg pools the frames read before the error and prints their score.
#
# Needs an FFmpeg built with the series that links libvmaf.so dynamically (the
# dev container's), libx264 in that FFmpeg and a C compiler. The libvmaf_cuda
# cases also need that filter and a CUDA device; without them they are
# reported as skipped and the CPU cases still decide the result. Errors are
# injected with fault_inject_libvmaf.c through LD_PRELOAD (shared helpers:
# filter_check_lib.sh).
#
#   <filter> baseline   no injection: exit 0, a score, a log of 24 frames
#   <filter> mid-run    the 5th vmaf_read_pictures() call fails (frame 4):
#                       non-zero exit, one error naming frame 4 and the error,
#                       no "VMAF score" line, no log file
#   libvmaf flush       the end-of-stream flush fails: no score, no log, and
#                       the error is named (uninit() cannot change the exit
#                       status; the script prints it)
#
# Environment: FFMPEG (default: ffmpeg), CC (default: cc), VMAF_INCLUDE
# (libvmaf's include directory when it is not on the compiler's default path).
#
# Exit codes: 0 pass, 1 fail, 77 skipped (no suitable FFmpeg).

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FFMPEG="${FFMPEG:-ffmpeg}"
CC="${CC:-cc}"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
# shellcheck source=ffmpeg-patches/test/filter_check_lib.sh
. "${HERE}/filter_check_lib.sh"

command -v "${FFMPEG}" >/dev/null 2>&1 || skip "no ${FFMPEG}"
has_filter libvmaf || skip "${FFMPEG} has no libvmaf filter"
build_fault_injector
encode_clips

run_cpu() { # $1 = tag: the libvmaf filter on software-decoded frames
  "${FFMPEG}" -hide_banner -nostdin -y -loglevel info \
    -i "${WORK}/dis.mp4" -i "${WORK}/ref.mp4" \
    -filter_complex "[0:v][1:v]libvmaf=log_fmt=json:log_path=${WORK}/$1.json" \
    -f null - >"${WORK}/$1.log" 2>&1
}

run_cuda() { # $1 = tag: the libvmaf_cuda filter on CUDA-decoded frames
  # NVDEC outputs nv12, which libvmaf_cuda does not take: scale_cuda converts
  # to yuv420p on the device.
  local graph="[0:v]scale_cuda=format=yuv420p[d];[1:v]scale_cuda=format=yuv420p[r]"
  "${FFMPEG}" -hide_banner -nostdin -y -loglevel info \
    -hwaccel cuda -hwaccel_output_format cuda -i "${WORK}/dis.mp4" \
    -hwaccel cuda -hwaccel_output_format cuda -i "${WORK}/ref.mp4" \
    -filter_complex "${graph};[d][r]libvmaf_cuda=log_fmt=json:log_path=${WORK}/$1.json" \
    -f null - >"${WORK}/$1.log" 2>&1
}

check_filter() { # $1 = filter name, $2 = run function
  local name="$1" run="$2" rc
  rc=0
  "${run}" "${name}-baseline" || rc=$?
  expect "${name} baseline: exit 0" test "${rc}" -eq 0
  expect "${name} baseline: a VMAF score ($(score_in "${WORK}/${name}-baseline.log"))" \
    test -n "$(score_in "${WORK}/${name}-baseline.log")"
  expect "${name} baseline: a log of 24 frames" \
    test "$(frames_in "${WORK}/${name}-baseline.json")" -eq 24

  rc=0
  LD_PRELOAD="${WORK}/libfault.so" VMAF_TEST_READ_FAIL_AT=5 VMAF_TEST_READ_FAIL_COUNT=1 \
    "${run}" "${name}-midrun" || rc=$?
  local log="${WORK}/${name}-midrun.log"
  expect "${name} mid-run error: non-zero exit (${rc})" test "${rc}" -ne 0
  expect "${name} mid-run error: one error naming frame 4 and the error" \
    count_is 1 "vmaf_read_pictures of frame 4 failed (Input/output error)" "${log}"
  expect "${name} mid-run error: no VMAF score printed" lacks "VMAF score" "${log}"
  expect "${name} mid-run error: no log written" test ! -e "${WORK}/${name}-midrun.json"
}

check_filter libvmaf run_cpu

rc=0
LD_PRELOAD="${WORK}/libfault.so" VMAF_TEST_FLUSH_FAIL=1 run_cpu libvmaf-flush || rc=$?
echo "libvmaf flush failure: exit ${rc} (uninit() cannot set the exit status)"
expect "libvmaf flush failure: the error is named" \
  grep -q "flushing libvmaf after frame 23 failed (Input/output error)" "${WORK}/libvmaf-flush.log"
expect "libvmaf flush failure: no VMAF score printed" lacks "VMAF score" "${WORK}/libvmaf-flush.log"
expect "libvmaf flush failure: no log written" test ! -e "${WORK}/libvmaf-flush.json"

if ! has_filter libvmaf_cuda; then
  echo "SKIP: libvmaf_cuda cases: ${FFMPEG} has no libvmaf_cuda filter"
elif ! run_cuda cuda-probe; then
  echo "SKIP: libvmaf_cuda cases: the filter does not run here (no CUDA device?)"
  sed -n '/libvmaf\|cuda\|CUDA/p' "${WORK}/cuda-probe.log" | tail -n 4
else
  check_filter libvmaf_cuda run_cuda
fi

if [ "${fail}" -ne 0 ]; then
  for tag in libvmaf-midrun libvmaf-flush libvmaf_cuda-midrun; do
    [ -e "${WORK}/${tag}.log" ] || continue
    echo "---- ${tag}.log (filter lines)" >&2
    sed -n '/libvmaf\|VMAF\|Error/p' "${WORK}/${tag}.log" | tail -n 8 >&2
  done
fi
exit "${fail}"
