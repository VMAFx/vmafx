#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# check-sycl-import-retry.sh — the libvmaf_sycl filter retries a failed VA
# surface import a bounded number of times and then stops with an error that
# names the frame; it never passes a frame through unscored (patch 0005,
# T-FFMPEG-SYCL-FILTER-IMPORT-FAILURE-SKIPS-FRAME-2026-10-05).
#
# Needs an FFmpeg built with the series and --enable-libvmaf-sycl that links
# libvmaf.so dynamically (the dev container's), an Intel GPU with QSV decode,
# a C compiler and libx264 in that FFmpeg. The failures are injected with
# fault_inject_libvmaf.c through LD_PRELOAD, so no libvmaf hook exists for
# them. Shared helpers: filter_check_lib.sh.
#
#   baseline     no injection: every frame scored
#   transient    calls 5 and 6 fail (the reference import of frame 2, twice):
#                the third try succeeds, the run exits 0 with the baseline's
#                frame count and pooled score
#   persistent   every call from 5 on fails: the run exits non-zero, the
#                error names frame 2, and no "VMAF score" line is printed
#   two displays the two decoders run on two VA devices (two displays on one
#                GPU): every frame equals the one-display run (the reference
#                surfaces used to be imported with the distorted input's
#                display, and named other surfaces there)
#   odd height   the software path (no QSV) on 576x323 frames: psnr_cb and
#                psnr_cr equal the CPU libvmaf filter's on every frame (the
#                last chroma row used to stay zero)
#
# Environment: FFMPEG (default: ffmpeg), VA_DEVICE (default: the first render
# node QSV opens), CC (default: cc), VMAF_INCLUDE (libvmaf's include directory
# when it is not on the compiler's default path).
#
# Exit codes: 0 pass, 1 fail, 77 skipped (no suitable FFmpeg or device).

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FFMPEG="${FFMPEG:-ffmpeg}"
CC="${CC:-cc}"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
# shellcheck source=ffmpeg-patches/test/filter_check_lib.sh
. "${HERE}/filter_check_lib.sh"

command -v "${FFMPEG}" >/dev/null 2>&1 || skip "no ${FFMPEG}"
has_filter libvmaf_sycl || skip "${FFMPEG} has no libvmaf_sycl filter"
build_fault_injector
encode_clips

run_filter() { # $1 = device, $2 = tag, $3 = second VA device (optional)
  local dev="$1" tag="$2" ref_va="va0" two=()
  if [ -n "${3:-}" ]; then
    two=(-init_hw_device "vaapi=va1:$3")
    ref_va="va1"
  fi
  "${FFMPEG}" -hide_banner -nostdin -y -loglevel info \
    -init_hw_device "vaapi=va0:${dev}" "${two[@]}" \
    -init_hw_device qsv=qsv_dis@va0 -init_hw_device "qsv=qsv_ref@${ref_va}" \
    -hwaccel qsv -hwaccel_device qsv_dis -hwaccel_output_format qsv -c:v h264_qsv \
    -i "${WORK}/dis.mp4" \
    -hwaccel qsv -hwaccel_device qsv_ref -hwaccel_output_format qsv -c:v h264_qsv \
    -i "${WORK}/ref.mp4" \
    -filter_complex "[0:v][1:v]libvmaf_sycl=log_fmt=json:log_path=${WORK}/${tag}.json" \
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
    if run_filter "${dev}" probe && grep -q "QSV zero-copy path" "${WORK}/probe.log"; then
      echo "${dev}"
      return 0
    fi
  done
  return 1
}

DEV="$(pick_device)" || skip "no render node where libvmaf_sycl runs the QSV zero-copy path"

# shellcheck disable=SC2329  # called through expect()
same_frames() { # $1 $2 = JSON logs; 0 when every frame's vmaf is equal
  python3 - "$1" "$2" <<'PY'
import json, sys
a, b = (json.load(open(f))["frames"] for f in sys.argv[1:3])
same = len(a) == len(b) and all(x["metrics"]["vmaf"] == y["metrics"]["vmaf"] for x, y in zip(a, b))
sys.exit(0 if same else 1)
PY
}

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

base_rc=0
run_filter "${DEV}" baseline || base_rc=$?
base_frames="$(frames_in "${WORK}/baseline.json")"
base_score="$(score_in "${WORK}/baseline.log")"
expect "baseline exits 0" test "${base_rc}" -eq 0
expect "baseline scores ${base_frames} frames (VMAF ${base_score})" test "${base_frames}" -gt 0 -a -n "${base_score}"

trans_rc=0
LD_PRELOAD="${WORK}/libfault.so" VMAF_TEST_IMPORT_FAIL_AT=5 VMAF_TEST_IMPORT_FAIL_COUNT=2 \
  run_filter "${DEV}" transient || trans_rc=$?
expect "transient failure: exit 0" test "${trans_rc}" -eq 0
expect "transient failure: every frame scored (${base_frames})" \
  test "$(frames_in "${WORK}/transient.json")" = "${base_frames}"
expect "transient failure: the baseline's pooled score" \
  test "$(score_in "${WORK}/transient.log")" = "${base_score}"
expect "transient failure: the retry is logged with the frame" grep -q "frame 2" "${WORK}/transient.log"
expect "transient failure: no frame skipped" lacks "skipping frame" "${WORK}/transient.log"

pers_rc=0
LD_PRELOAD="${WORK}/libfault.so" VMAF_TEST_IMPORT_FAIL_AT=5 VMAF_TEST_IMPORT_FAIL_COUNT=100000 \
  run_filter "${DEV}" persistent || pers_rc=$?
expect "persistent failure: non-zero exit (${pers_rc})" test "${pers_rc}" -ne 0
expect "persistent failure: the error names frame 2" \
  grep -q "cannot import the reference VA surface .* of frame 2" "${WORK}/persistent.log"
expect "persistent failure: no VMAF score printed" lacks "VMAF score" "${WORK}/persistent.log"

two_rc=0
run_filter "${DEV}" twodisplays "${DEV}" || two_rc=$?
expect "two VA displays: exit 0" test "${two_rc}" -eq 0
expect "two VA displays: every frame equals the one-display run" \
  same_frames "${WORK}/baseline.json" "${WORK}/twodisplays.json"

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
run_raw() { # $1 = filter (libvmaf or libvmaf_sycl), $2 = tag
  local raw=(-f rawvideo -pix_fmt yuv420p -s 576x323 -r 24)
  "${FFMPEG}" -hide_banner -nostdin -y -loglevel info "${raw[@]}" -i "${WORK}/odis.yuv" \
    "${raw[@]}" -i "${WORK}/oref.yuv" \
    -filter_complex "[0:v][1:v]$1=feature=name=psnr:log_fmt=json:log_path=${WORK}/$2.json" \
    -f null - >"${WORK}/$2.log" 2>&1
}
odd_rc=0
run_raw libvmaf_sycl oddsycl || odd_rc=$?
expect "odd height, software path: exit 0" test "${odd_rc}" -eq 0
expect "odd height, software path: 576x323 frames reach the filter" \
  grep -q "software path, format=yuv420p bpc=8 576x323" "${WORK}/oddsycl.log"
cpu_rc=0
run_raw libvmaf oddcpu || cpu_rc=$?
expect "odd height, CPU libvmaf: exit 0" test "${cpu_rc}" -eq 0
expect "odd height, software path: psnr_y / psnr_cb / psnr_cr equal the CPU's on every frame" \
  same_psnr "${WORK}/oddsycl.json" "${WORK}/oddcpu.json"

if [ "${fail}" -ne 0 ]; then
  for tag in transient persistent twodisplays oddsycl; do
    echo "---- ${tag}.log (filter lines)" >&2
    sed -n '/libvmaf/p' "${WORK}/${tag}.log" | tail -n 8 >&2
  done
fi
exit "${fail}"
