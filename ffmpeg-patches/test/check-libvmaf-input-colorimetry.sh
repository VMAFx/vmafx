#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# check-libvmaf-input-colorimetry.sh — an HDR-tagged input reaches libvmaf's
# conversion through the libvmaf filter (patch 0022, ADR-2093).
#
# A copy of model/vmaf_v0.6.1.json gets a `conversion_target` (BT.2020 / PQ /
# ICtCp, limited range). The filter is run on two 10-bit clips:
#   tagged     range tv, bt2020, smpte2084, bt2020nc on both inputs: exit 0, a
#              score (libvmaf converted both pictures to the target)
#   untagged   no colour tags: libvmaf refuses with its "requires source
#              colorimetry" error naming the missing attributes, non-zero exit
#   plain      the unmodified model on the untagged clips: exit 0, a score
#              (behaviour without a conversion_target is unchanged)
#
# Needs an FFmpeg built with the series, linked against a libvmaf built with
# -Denable_zimg=true, and python3. Without zimg the tagged case is reported
# as skipped (libvmaf returns -ENOTSUP). Environment: FFMPEG (default
# ffmpeg), VMAF_MODEL_DIR (default: model/ of this repository).
#
# Exit codes: 0 pass, 1 fail, 77 skipped.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FFMPEG="${FFMPEG:-ffmpeg}"
CC="${CC:-cc}"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
# shellcheck source=ffmpeg-patches/test/filter_check_lib.sh disable=SC1091
. "${HERE}/filter_check_lib.sh"

MODEL_DIR="${VMAF_MODEL_DIR:-${HERE}/../../model}"
command -v "${FFMPEG}" >/dev/null 2>&1 || skip "no ${FFMPEG}"
command -v python3 >/dev/null 2>&1 || skip "no python3"
has_filter libvmaf || skip "${FFMPEG} has no libvmaf filter"
[ -f "${MODEL_DIR}/vmaf_v0.6.1.json" ] || skip "no ${MODEL_DIR}/vmaf_v0.6.1.json"

python3 - "${MODEL_DIR}/vmaf_v0.6.1.json" "${WORK}/hdr_model.json" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
m["model_dict"]["conversion_target"] = {
    "colorspace": {"range": "limited", "primaries": "bt2020", "trc": "smpte2084", "matrix": "ictcp"}
}
json.dump(m, open(sys.argv[2], "w"))
PY

# Two 10-bit clips without colour tags (nut/ffv1 does not keep them): a
# reference and a noisier distorted one. The tags are set in the filter graph.
clip() { # $1 = out, $2 = noise filter (may be empty)
  "${FFMPEG}" -hide_banner -loglevel error -y -f lavfi \
    -i "testsrc2=size=256x144:rate=5:duration=2,format=yuv420p10le${2:+,$2}" \
    -f nut -c:v ffv1 "$1"
}
clip "${WORK}/ref.nut" ""
clip "${WORK}/dis.nut" "noise=alls=12:allf=t"
TAGS="setparams=range=tv:color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc"

run() { # $1 = tag, $2 = model path, $3 = filter applied to both inputs (may be empty)
  local pre="${3:+,$3}"
  "${FFMPEG}" -hide_banner -nostdin -y -loglevel info -i "${WORK}/dis.nut" -i "${WORK}/ref.nut" \
    -filter_complex "[0:v]null${pre}[d];[1:v]null${pre}[r];[d][r]libvmaf=model=path=$2" \
    -f null - >"${WORK}/$1.log" 2>&1
}

rc=0
run tagged "${WORK}/hdr_model.json" "${TAGS}" || rc=$?
if grep -q "built without zimg" "${WORK}/tagged.log"; then
  echo "SKIP: tagged case: libvmaf was built without zimg"
else
  expect "tagged: exit 0 (${rc})" test "${rc}" -eq 0
  expect "tagged: a VMAF score" grep -q "VMAF score" "${WORK}/tagged.log"
  expect "tagged: libvmaf did not ask for colorimetry" lacks "requires source colorimetry" "${WORK}/tagged.log"
fi

rc=0
run untagged "${WORK}/hdr_model.json" "" || rc=$?
expect "untagged: non-zero exit (${rc})" test "${rc}" -ne 0
expect "untagged: libvmaf names the missing colorimetry" \
  grep -q "requires source colorimetry" "${WORK}/untagged.log"
expect "untagged: no VMAF score" lacks "VMAF score" "${WORK}/untagged.log"

rc=0
run plain "${MODEL_DIR}/vmaf_v0.6.1.json" "" || rc=$?
expect "no target, untagged: exit 0 (${rc})" test "${rc}" -eq 0
expect "no target, untagged: a VMAF score" grep -q "VMAF score" "${WORK}/plain.log"

# shellcheck disable=SC2154  # `fail` is set by filter_check_lib.sh
exit "${fail}"
