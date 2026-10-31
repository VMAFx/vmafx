#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# check-vmafx-import-failure.sh — the vmafx filter stops immediately on import
# failure, logs an error naming the frame and input, and prints no pooled
# score line; it never passes a frame through unscored (patch 0004).
#
# Bounded retry on transient acquisition delay lives in core under decision D8
# (vmafx_context_import_frame(), core/src/vmafx/frame_import_admit.c, proven
# by core/test/test_vmafx_import_fence). The filter itself performs no retry
# loop, so no transient retry case is tested here; D8 belongs to the core test.
#
# Two-display case dropped: vf_vmafx.c (lines 794-835) imports AV_PIX_FMT_DRM_PRIME
# dma-bufs directly into a single VMAFx device (s->vdev opened in drm_open()).
# There is no per-input VA display abstraction in vf_vmafx.c.
#
# Needs an FFmpeg built with the series and --enable-libvmafx that links
# libvmafx.so dynamically, a hardware device with a path to DRM PRIME frames
# (VAAPI decode mapped to drm_prime via hwmap), and a C compiler. The failures
# are injected into vmafx_context_import_frame() with fault_inject_libvmaf.c
# through LD_PRELOAD. Shared helpers: filter_check_lib.sh.
#
#   baseline     no injection: every frame scored on the device
#   persistent   every call from 5 on fails: the run exits non-zero, the
#                error names frame 2, and no "VMAF score" line is printed
#   odd height   the software path on 576x323 frames: psnr_cb and psnr_cr equal
#                the CPU vmafx filter's on every frame (tests (h + 1) / 2 chroma rows)
#
# Environment: FFMPEG (default: ffmpeg), VA_DEVICE (default: the first render
# node where VAAPI decode with hwmap to DRM PRIME runs; set VA_DEVICE=host for host
# import testing), VMAFX_DRM_BACKEND (default: sycl; hip valid for AMD nodes),
# CC (default: cc), VMAF_INCLUDE (libvmafx's include directory when it is not
# on the compiler's default path).
#
# Exit codes: 0 pass, 1 fail, 77 skipped (no suitable FFmpeg or device).

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FFMPEG="${FFMPEG:-ffmpeg}"
CC="${CC:-cc}"
VMAFX_DRM_BACKEND="${VMAFX_DRM_BACKEND:-sycl}"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
# shellcheck source=ffmpeg-patches/test/filter_check_lib.sh disable=SC1091
. "${HERE}/filter_check_lib.sh"
# The library starts the failure flag; set here so the use below is visibly assigned.
fail="${fail:-0}"

OPTIONS="${HERE}/../src/vf_vmafx_options.h"
[ -f "${OPTIONS}" ] || {
  echo "FAIL: missing ${OPTIONS}" >&2
  exit 1
}
for opt in backend sycl log_fmt log_path; do
  grep -q "\"${opt}\"" "${OPTIONS}" || {
    echo "FAIL: missing option '${opt}' in ${OPTIONS}" >&2
    exit 1
  }
done

command -v "${FFMPEG}" >/dev/null 2>&1 || skip "no ${FFMPEG}"
has_filter vmafx || skip "${FFMPEG} has no vmafx filter"
build_fault_injector
encode_clips

run_filter() { # $1 = device, $2 = tag
  local dev="$1" tag="$2"
  if [ "${dev}" = "host" ]; then
    "${FFMPEG}" -hide_banner -nostdin -y -loglevel info \
      -i "${WORK}/dis.mp4" -i "${WORK}/ref.mp4" \
      -filter_complex "[0:v]format=nv12[d];[1:v]format=nv12[r];[d][r]vmafx=backend=cpu:log_fmt=json:log_path=${WORK}/${tag}.json" \
      -f null - >"${WORK}/${tag}.log" 2>&1
    return $?
  fi
  "${FFMPEG}" -hide_banner -nostdin -y -loglevel info \
    -init_hw_device "vaapi=va0:${dev}" \
    -hwaccel vaapi -hwaccel_device va0 -hwaccel_output_format vaapi \
    -i "${WORK}/dis.mp4" \
    -hwaccel vaapi -hwaccel_device va0 -hwaccel_output_format vaapi \
    -i "${WORK}/ref.mp4" \
    -filter_complex "[0:v]hwmap=mode=read+direct,format=drm_prime[d];[1:v]hwmap=mode=read+direct,format=drm_prime[r];[d][r]vmafx=backend=${VMAFX_DRM_BACKEND}:log_fmt=json:log_path=${WORK}/${tag}.json" \
    -f null - >"${WORK}/${tag}.log" 2>&1
}

pick_device() {
  local dev
  if [ -n "${VA_DEVICE:-}" ]; then
    echo "${VA_DEVICE}"
    return 0
  fi
  for dev in /dev/dri/renderD*; do
    [ -e "${dev}" ] || continue
    if run_filter "${dev}" probe 2>/dev/null &&
      grep -qE "vmafx frames: [1-9][0-9]* imported on the device" "${WORK}/probe.log" 2>/dev/null; then
      echo "${dev}"
      return 0
    fi
  done
  return 1
}

DEV="$(pick_device)" || skip "no render node or decoder where vmafx has a path to DRM PRIME frames in ${FFMPEG}"

# shellcheck disable=SC2329  # called through expect()
same_psnr() { # $1 $2 = JSON logs; 0 when psnr_y / psnr_cb / psnr_cr are equal on every frame
  python3 - "$1" "$2" <<'PY'
import json, sys
a, b = (json.load(open(f))["frames"] for f in sys.argv[1:3])
keys = ("psnr_y", "psnr_cb", "psnr_cr")
same = len(a) == len(b) > 0 and all(x["metrics"][k] == y["metrics"][k] for x, y in zip(a, b) for k in keys)
sys.exit(0 if same else 1)
PY
}

# shellcheck disable=SC2329  # called through expect()
odd_host_frames() { # $1 = JSON log, $2 = filter log: 0 when 6 frames and host path matched
  [ "$(frames_in "$1")" -eq 6 ] && grep -q "0 imported on the device, 12 host" "$2"
}

base_rc=0
run_filter "${DEV}" baseline || base_rc=$?
base_frames="$(frames_in "${WORK}/baseline.json")"
base_score="$(score_in "${WORK}/baseline.log")"
expect "baseline exits 0" test "${base_rc}" -eq 0
expect "baseline scores ${base_frames} frames (VMAF ${base_score})" test "${base_frames}" -gt 0 -a -n "${base_score}"
if [ "${DEV}" = host ]; then
  expect "baseline imports every frame on the host" \
    grep -qE "vmafx frames: 0 imported on the device, [1-9][0-9]* host, 0 downloaded" "${WORK}/baseline.log"
else
  expect "baseline imports every frame on the device" \
    grep -qE "vmafx frames: [1-9][0-9]* imported on the device, 0 host, 0 downloaded" "${WORK}/baseline.log"
fi

pers_rc=0
LD_PRELOAD="${WORK}/libfault.so" VMAF_TEST_IMPORT_FAIL_AT=5 VMAF_TEST_IMPORT_FAIL_COUNT=100000 \
  run_filter "${DEV}" persistent || pers_rc=$?
expect "persistent failure: non-zero exit (${pers_rc})" test "${pers_rc}" -ne 0
expect "persistent failure: the error names frame 2" \
  grep -q "frame 2" "${WORK}/persistent.log"
expect "persistent failure: no VMAF score printed" lacks "VMAF score" "${WORK}/persistent.log"

# Odd-height 4:2:0 frames reach a filter only from a raw source (encoders and
# testsrc2 round the height down): six 576x323 frames per side.
python3 - "${WORK}" <<'PY'
import sys
w, h = 576, 323
cw, ch = (w + 1) // 2, (h + 1) // 2
for name, salt in (("oref.yuv", 0), ("odis.yuv", 3)):
    with open(f"{sys.argv[1]}/{name}", "wb") as out:
        for fr in range(6):
            out.write(bytes((x * 3 + y * 5 + fr * 7 + salt * (x % 5)) & 0xFF for y in range(h) for x in range(w)))
            out.write(bytes((x * 7 + y * 3 + fr + salt * (y % 3)) & 0xFF for y in range(ch) for x in range(cw)))
            out.write(bytes((x * 5 + y * 9 + fr * 2 + salt * ((x + y) % 4)) & 0xFF for y in range(ch) for x in range(cw)))
PY

run_raw() { # $1 = filter args, $2 = tag
  local raw=(-f rawvideo -pix_fmt yuv420p -s 576x323 -r 24)
  "${FFMPEG}" -hide_banner -nostdin -y -loglevel info "${raw[@]}" -i "${WORK}/odis.yuv" \
    "${raw[@]}" -i "${WORK}/oref.yuv" \
    -filter_complex "[0:v][1:v]vmafx=$1:feature=name=psnr:log_fmt=json:log_path=${WORK}/$2.json" \
    -f null - >"${WORK}/$2.log" 2>&1
}

odd_rc=0
run_raw "backend=sycl" oddsycl || odd_rc=$?
if [ "${odd_rc}" -eq 0 ]; then
  expect "odd height, software path: exit 0" test "${odd_rc}" -eq 0
  expect "odd height, software path: 576x323 frames reach the filter on the host path" \
    odd_host_frames "${WORK}/oddsycl.json" "${WORK}/oddsycl.log"
  cpu_rc=0
  run_raw "backend=cpu" oddcpu || cpu_rc=$?
  expect "odd height, CPU vmafx: exit 0" test "${cpu_rc}" -eq 0
  expect "odd height, software path: psnr_y / psnr_cb / psnr_cr equal the CPU's on every frame" \
    same_psnr "${WORK}/oddsycl.json" "${WORK}/oddcpu.json"
else
  echo "SKIP: odd height SYCL path not supported by this build"
fi

if [ "${fail}" -ne 0 ]; then
  for tag in persistent oddsycl; do
    if [ -f "${WORK}/${tag}.log" ]; then
      echo "---- ${tag}.log (filter lines)" >&2
      sed -n '/vmafx/p' "${WORK}/${tag}.log" | tail -n 8 >&2
    fi
  done
fi
exit "${fail}"
