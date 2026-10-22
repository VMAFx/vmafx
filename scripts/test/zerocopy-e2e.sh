#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# zerocopy-e2e.sh - libvmaf_sycl zero-copy end-to-end harness.
#
# Runs inside the SYCL toolchain image (scripts/test/sycl-dev-container.sh exec)
# on an Intel GPU. For every clip and bit depth it QSV-encodes a ref/dis pair,
# then runs every case through three legs and compares them with
# scripts/test/zerocopy_e2e_compare.py:
#
#   cpu   software decode -> libvmaf              (reference)
#   host  software decode -> libvmaf_sycl         (host upload, SYCL twins)
#   zc    QSV decode      -> libvmaf_sycl         (zero-copy)
#
# A case the CPU extractor cannot run (motion_uv: integer motion with
# motion_add_uv) is declared reference=host by the comparator: no cpu leg is run
# and zero-copy is compared with host upload only.
#
# Every leg writes its scores with score_fmt=%.17g (SCORE_FMT, patch 0016), so the
# comparator sees full doubles rather than six decimals.
#
# Every leg runs both inputs to their end. Never cut a leg with the output options
# -frames:v or -t: FFmpeg enforces them after the filtergraph, so any libvmaf*
# filter may score one more pair than ffmpeg outputs (a race with -frames:v,
# every run with -t). To score the first N frames, put trim=end_frame=N on both
# inputs (docs/usage/ffmpeg.md).
#
# Usage:
#   scripts/test/sycl-dev-container.sh exec bash scripts/test/zerocopy-e2e.sh \
#     --stage N --out DIR [--clips src01,checkerboard] [--depths 8,10] \
#     [--cases a,b,...] [--yuv /yuv] [--bench] [--repeat N] [--render-node PATH]
#
# --render-node (or RENDER_NODE) names the Intel GPU's DRM render node; the QSV
# encode and the zero-copy decode both run on it. Default: the first render node
# whose PCI vendor is Intel (0x8086), so a host with more GPUs needs no setting.
#
# Case files are written to DIR as <clip>_<depth>bit__<case>.<leg>.{json,rc,err};
# DIR must not be shared between runs of different --depths. --repeat N runs the
# zero-copy leg N times (extra runs as <leg> zc-r2..zc-rN); a parity case fails as
# zc-nondeterministic when any run differs from host upload. Exit status is the
# comparator's: 0 only when no case failed and every host leg is bit-exact.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPARE="$HERE/zerocopy_e2e_compare.py"
STAGE=""
OUT=""
CLIPS="src01,checkerboard"
DEPTHS="8,10"
CASES=""
YUV="/yuv"
BENCH=0
REPEAT=1
CB_REPEAT="${CB_REPEAT:-20}"
LEG_TIMEOUT="${LEG_TIMEOUT:-600}"
SCORE_FMT="${SCORE_FMT:-%.17g}"
RENDER_NODE="${RENDER_NODE:-}"

usage() {
  cat >&2 <<'EOF'
usage: zerocopy-e2e.sh --stage {1,2,3} --out DIR [--clips list] [--depths list]
                       [--cases list] [--yuv DIR] [--bench] [--repeat N]
                       [--render-node PATH]
EOF
  exit 2
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --stage)
      STAGE="${2:?}"
      shift 2
      ;;
    --out)
      OUT="${2:?}"
      shift 2
      ;;
    --clips)
      CLIPS="${2:?}"
      shift 2
      ;;
    --depths)
      DEPTHS="${2:?}"
      shift 2
      ;;
    --cases)
      CASES="${2:?}"
      shift 2
      ;;
    --yuv)
      YUV="${2:?}"
      shift 2
      ;;
    --bench)
      BENCH=1
      shift
      ;;
    --repeat)
      REPEAT="${2:?}"
      shift 2
      ;;
    --render-node)
      RENDER_NODE="${2:?}"
      shift 2
      ;;
    *) usage ;;
  esac
done
case "$STAGE" in 1 | 2 | 3) ;; *) usage ;; esac
[ -n "$OUT" ] || usage
case "$REPEAT" in '' | *[!0-9]* | 0) usage ;; esac
# first_intel_render_node: the first /dev/dri/renderD* of PCI vendor 0x8086.
first_intel_render_node() {
  local v
  for v in /sys/class/drm/renderD*/device/vendor; do
    [ -r "$v" ] || continue
    if [ "$(cat "$v")" = 0x8086 ]; then
      v="${v%/device/vendor}"
      echo "/dev/dri/${v##*/}"
      return 0
    fi
  done
  return 1
}
if [ -z "$RENDER_NODE" ] && ! RENDER_NODE="$(first_intel_render_node)"; then
  echo "no Intel render node found; pass --render-node" >&2
  exit 2
fi
if [ ! -c "$RENDER_NODE" ]; then
  echo "render node is not a character device: $RENDER_NODE" >&2
  exit 2
fi
QSV_INIT=(-init_hw_device "vaapi=va0:$RENDER_NODE"
  -init_hw_device qsv=qr@va0 -init_hw_device qsv=qd@va0)
OUT="$(realpath -m "$OUT")"
mkdir -p "$OUT/media"
# Stale case files from an earlier run would be compared again.
rm -f "$OUT"/*__*.cpu.* "$OUT"/*__*.host.* "$OUT"/*__*.zc.* "$OUT"/*__*.zc-r*.*

# clip -> "WxH ref.yuv dis.yuv repeat"
clip_info() {
  case "$1" in
    src01) echo "576x324 src01_hrc00_576x324.yuv src01_hrc01_576x324.yuv 1" ;;
    checkerboard) echo "1920x1080 checkerboard_1920_1080_10_3_0_0.yuv checkerboard_1920_1080_10_3_1_0.yuv $CB_REPEAT" ;;
    *)
      echo "unknown clip: $1" >&2
      exit 2
      ;;
  esac
}

# repeat_file SRC DST N: DST is N back-to-back copies of SRC.
repeat_file() {
  local src="$1" dst="$2" n="$3" i
  : >"$dst"
  for ((i = 0; i < n; i++)); do cat "$src" >>"$dst"; done
}

# encode IN.yuv WxH DEPTH OUT.mp4: hevc_qsv on RENDER_NODE, NV12 for 8 bit, P010
# for 10 bit.
encode() {
  local in="$1" size="$2" depth="$3" out="$4" pf prof
  if [ "$depth" = 8 ]; then
    pf=nv12
    prof=main
  else
    pf=p010le
    prof=main10
  fi
  if ! timeout "$LEG_TIMEOUT" ffmpeg -hide_banner -loglevel error -y \
    -init_hw_device "vaapi=ve:$RENDER_NODE" -init_hw_device qsv=qe@ve -filter_hw_device qe \
    -f rawvideo -pix_fmt yuv420p -s "$size" -i "$in" \
    -vf "format=$pf,hwupload=extra_hw_frames=16" \
    -c:v hevc_qsv -profile:v "$prof" -global_quality 22 "$out"; then
    echo "ENCODE FAILED $out" >&2
    exit 1
  fi
}

# run_leg BASE ffmpeg-args...: runs ffmpeg, records the exit code in BASE.rc.
run_leg() {
  local base="$1" rc=0
  shift
  rm -f "$base".json "$base".rc "$base".err
  timeout "$LEG_TIMEOUT" ffmpeg -hide_banner -nostats -y "$@" \
    >"$base.out" 2>"$base.err" || rc=$?
  echo "$rc" >"$base.rc"
}

# filter_option KIND ARG: the libvmaf options selecting the case. A feature case
# switches the default model off (empty model=) so only that feature is measured.
filter_option() {
  if [ "$1" = model ]; then printf 'model=%s' "$2"; else printf "model=:feature='%s'" "$2"; fi
}

# run_zc BASE DIS REF FILTER_OPT BENCHFLAG: one zero-copy run.
run_zc() {
  local base="$1" dis="$2" ref="$3" opt="$4" benchflag="$5"
  # The decoders do not share a QSV device: one per input (docs/backends/sycl/overview.md).
  run_leg "$base" "${QSV_INIT[@]}" \
    -hwaccel qsv -hwaccel_output_format qsv -hwaccel_device qd -i "$dis" \
    -hwaccel qsv -hwaccel_output_format qsv -hwaccel_device qr -i "$ref" \
    ${benchflag:+"$benchflag"} \
    -lavfi "[0:v][1:v]libvmaf_sycl=${opt}:score_fmt=${SCORE_FMT}:log_fmt=json:log_path=${base}.json" -f null -
}

# run_case TAG DIS REF ID KIND ARG BENCHFLAG REFERENCE
run_case() {
  local tag="$1" dis="$2" ref="$3" id="$4" kind="$5" arg="$6" benchflag="$7" reference="$8"
  local opt base
  opt="$(filter_option "$kind" "$arg")"
  base="$OUT/${tag}__${id}"
  if [ "$reference" = cpu ]; then
    run_leg "$base.cpu" -i "$dis" -i "$ref" \
      -lavfi "[0:v][1:v]libvmaf=${opt}:score_fmt=${SCORE_FMT}:log_fmt=json:log_path=${base}.cpu.json" -f null -
  fi
  run_leg "$base.host" -i "$dis" -i "$ref" \
    -lavfi "[0:v][1:v]libvmaf_sycl=${opt}:score_fmt=${SCORE_FMT}:log_fmt=json:log_path=${base}.host.json" -f null -
  run_zc "$base.zc" "$dis" "$ref" "$opt" "$benchflag"
  local k
  for ((k = 2; k <= REPEAT; k++)); do
    run_zc "$base.zc-r$k" "$dis" "$ref" "$opt" ""
  done
}

# print_bench CLIP DEPTH BASE: ZC-E2E BENCH line from the zero-copy leg's -benchmark output.
print_bench() {
  local clip="$1" depth="$2" base="$3" rtime frames
  rtime="$(grep -o 'rtime=[0-9.]*s' "$base.zc.err" | tail -n1 | sed 's/rtime=//; s/s$//')"
  frames="$(python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["frames"]))' \
    "$base.zc.json" 2>/dev/null || echo 0)"
  if [ -z "$rtime" ] || [ "$frames" = 0 ]; then
    echo "ZC-E2E BENCH $clip $depth unavailable (no timing or no frames)"
    return 0
  fi
  python3 - "$clip" "$depth" "$frames" "$rtime" <<'PY'
import sys
clip, depth, frames, rtime = sys.argv[1:5]
print(f"ZC-E2E BENCH {clip} {depth} frames={frames} rtime={rtime} fps={int(frames) / float(rtime):.2f}")
PY
}

# Case table: id <TAB> kind <TAB> filter argument <TAB> stage <TAB> reference leg
# (single source: the comparator).
LIST_ARGS=(--list)
[ -z "$CASES" ] || LIST_ARGS+=(--cases "$CASES")
mapfile -t CASE_ROWS < <(python3 "$COMPARE" "${LIST_ARGS[@]}")
CASE_IDS=()
for row in "${CASE_ROWS[@]}"; do CASE_IDS+=("${row%%$'\t'*}"); done

IFS=, read -r -a CLIP_LIST <<<"$CLIPS"
IFS=, read -r -a DEPTH_LIST <<<"$DEPTHS"
for clip in "${CLIP_LIST[@]}"; do
  read -r size refname disname repeat <<<"$(clip_info "$clip")"
  repeat_file "$YUV/$refname" "$OUT/media/${clip}_ref.yuv" "$repeat"
  repeat_file "$YUV/$disname" "$OUT/media/${clip}_dis.yuv" "$repeat"
  for depth in "${DEPTH_LIST[@]}"; do
    tag="${clip}_${depth}bit"
    encode "$OUT/media/${clip}_ref.yuv" "$size" "$depth" "$OUT/media/${tag}_ref.mp4"
    encode "$OUT/media/${clip}_dis.yuv" "$size" "$depth" "$OUT/media/${tag}_dis.mp4"
    for row in "${CASE_ROWS[@]}"; do
      IFS=$'\t' read -r id kind arg _stage reference <<<"$row"
      benchflag=""
      if [ "$BENCH" = 1 ] && [ "$id" = "model-vmaf_v0.6.1" ] && [ "$clip" = checkerboard ]; then
        benchflag="-benchmark"
      fi
      echo "ZC-E2E RUN $tag $id"
      run_case "$tag" "$OUT/media/${tag}_dis.mp4" "$OUT/media/${tag}_ref.mp4" \
        "$id" "$kind" "$arg" "$benchflag" "$reference"
      if [ -n "$benchflag" ]; then print_bench "$clip" "$depth" "$OUT/${tag}__${id}"; fi
    done
  done
done

IDS_CSV="$(
  IFS=,
  echo "${CASE_IDS[*]}"
)"
python3 "$COMPARE" --stage "$STAGE" --dir "$OUT" --cases "$IDS_CSV"
