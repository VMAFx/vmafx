#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# filter_check_lib.sh — helpers shared by the FFmpeg filter checks in this
# directory (check-sycl-import-retry.sh, check-libvmaf-no-score-after-error.sh).
# Sourced, not run. The caller sets `set -euo pipefail`, FFMPEG, CC, HERE and
# WORK (a fresh temporary directory) before sourcing it.

set -euo pipefail

# shellcheck disable=SC2034  # `fail` is read by the sourcing script
fail=0

skip() { # $1 = reason; exit 77 is the "skipped" code of the checks
  echo "SKIP: $1" >&2
  exit 77
}

# shellcheck disable=SC2034  # `fail` is read by the sourcing script
expect() { # $1 = label, rest = a command that must succeed
  local label="$1"
  shift
  if "$@"; then
    echo "PASS: ${label}"
  else
    echo "FAIL: ${label}" >&2
    fail=1
  fi
}

# shellcheck disable=SC2329  # called through expect()
lacks() { # $1 = pattern, $2 = file: 0 when the file has no matching line
  ! grep -q -- "$1" "$2"
}

# shellcheck disable=SC2329  # called through expect()
count_is() { # $1 = expected count, $2 = literal text, $3 = file: 0 when exactly $1 lines hold it
  local n
  n="$(awk -v text="$2" 'index($0, text) { c++ } END { print c + 0 }' "$3")"
  [ "${n}" -eq "$1" ]
}

has_filter() { # $1 = filter name: 0 when ${FFMPEG} lists it
  # The list goes to a variable first: `grep -q` on a pipe would end FFmpeg
  # with SIGPIPE, and pipefail would read that as "no filter".
  local filters
  filters="$("${FFMPEG}" -hide_banner -filters 2>/dev/null)" || return 1
  grep -q " $1 " <<<"${filters}"
}

build_fault_injector() { # ${WORK}/libfault.so from fault_inject_libvmaf.c, or skip
  command -v "${CC}" >/dev/null 2>&1 || skip "no C compiler (${CC})"
  "${CC}" -shared -fPIC -O2 -D_GNU_SOURCE ${VMAF_INCLUDE:+-I"${VMAF_INCLUDE}"} \
    -o "${WORK}/libfault.so" "${HERE}/fault_inject_libvmaf.c" -ldl ||
    skip "cannot build the fault injector"
}

encode_clips() { # ${WORK}/ref.mp4 and a blurred ${WORK}/dis.mp4: 24 H.264 frames, 576x324
  "${FFMPEG}" -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=576x324:rate=24 \
    -frames:v 24 -c:v libx264 -profile:v high -crf 10 -pix_fmt yuv420p "${WORK}/ref.mp4" ||
    skip "cannot encode the reference clip"
  "${FFMPEG}" -hide_banner -loglevel error -y -i "${WORK}/ref.mp4" -vf boxblur=2:1 \
    -c:v libx264 -profile:v high -crf 30 -pix_fmt yuv420p "${WORK}/dis.mp4" ||
    skip "cannot encode the distorted clip"
}

frames_in() { # pooled frame count of a JSON log, or -1 (no log, or not JSON)
  python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["frames"]))' "$1" 2>/dev/null ||
    echo -1
}

score_in() { # the filter's "VMAF score:" value, or nothing
  sed -n 's/.*VMAF score: \([0-9.]*\).*/\1/p' "$1" | tail -n 1
}
