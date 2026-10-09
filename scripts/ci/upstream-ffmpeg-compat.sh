#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# Unpatched upstream FFmpeg (FFMPEG_TAG from build-config.env) built against a
# libvmaf install, scored with its stock `libvmaf` filter, scores compared as
# exact text. Proves the compat libvmaf keeps source and binary compatibility
# for consumers that know nothing about this fork. See
# docs/development/upstream-consumers.md.
set -euo pipefail

# shellcheck source=scripts/ci/upstream-consumer-lib.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/upstream-consumer-lib.sh"

uc_defaults
uc_parse_common "$@"
if [ "${#UC_REST[@]}" -gt 0 ]; then
  case "${UC_REST[0]}" in
    -h | --help)
      uc_usage "upstream-ffmpeg-compat.sh" \
        "  --cuda                  also run the upstream libvmaf_cuda filter (skipped with a reason
                          when the host has no GPU runtime or the library has no CUDA)"
      exit 0
      ;;
    *) uc_die "unknown argument: ${UC_REST[0]}" ;;
  esac
fi
uc_validate

FF_TAG="$(uc_cfg FFMPEG_TAG)"
FF_REMOTE="$(uc_cfg FFMPEG_REMOTE)"
FF_ROOT="$WORK/ffmpeg-$FF_TAG"
FF_SRC="$FF_ROOT/src"
STATUS=0

# Common configure line: nothing but what a libvmaf score run needs. No patch
# is applied to the tree. The CUDA leg adds its own components below.
ff_configure_args() {
  printf '%s\n' --disable-everything --disable-autodetect --disable-doc \
    --disable-debug --disable-network --disable-x86asm --disable-programs \
    --enable-ffmpeg --enable-avcodec --enable-avformat --enable-avfilter \
    --enable-swscale --enable-swresample --enable-demuxer=rawvideo \
    --enable-decoder=rawvideo --enable-encoder=wrapped_avframe,rawvideo \
    --enable-muxer=null --enable-protocol=file \
    --enable-filter=libvmaf,scale,format,null,nullsink,trim,setpts --enable-libvmaf
}

ff_fetch() {
  if [ -d "$FF_SRC/.git" ] &&
    [ "$(uc_exact_tag "$FF_SRC")" = "$FF_TAG" ]; then
    uc_log "reusing FFmpeg $FF_TAG at $FF_SRC"
    return 0
  fi
  rm -rf "$FF_SRC"
  mkdir -p "$FF_ROOT"
  uc_log "cloning $FF_REMOTE at $FF_TAG (shallow)"
  timeout 600 git -c advice.detachedHead=false clone --quiet --depth 1 \
    --branch "$FF_TAG" "$FF_REMOTE" "$FF_SRC" || uc_die "cannot clone FFmpeg $FF_TAG"
}

# ff_build NAME [extra configure args...]: out-of-tree build in $FF_ROOT/NAME.
# A stamp of the prefix and the configure line decides whether to reconfigure.
ff_build() {
  local name="$1" dir stamp want
  shift
  dir="$FF_ROOT/$name"
  mapfile -t base_args < <(ff_configure_args)
  want="$PREFIX|${base_args[*]}|$*"
  stamp="$dir/.compat-stamp"
  if [ -x "$dir/ffmpeg" ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ]; then
    uc_log "reusing FFmpeg build $dir"
    return 0
  fi
  rm -rf "$dir"
  mkdir -p "$dir"
  uc_log "configuring FFmpeg ($name) against $PREFIX"
  (
    cd "$dir"
    PKG_CONFIG_PATH="$(uc_pkgpath "$PREFIX")" timeout 590 "$FF_SRC/configure" \
      "${base_args[@]}" "$@" >configure.log 2>&1
  ) || uc_die "FFmpeg configure failed (see $dir/configure.log)"
  grep -q 'define CONFIG_LIBVMAF_FILTER 1' "$dir/config_components.h" ||
    uc_die "libvmaf filter not enabled in $dir/config_components.h"
  uc_log "building ffmpeg (-j$JOBS)"
  (cd "$dir" && nice -n 10 timeout 590 make -j"$JOBS" ffmpeg >make.log 2>&1) ||
    uc_die "FFmpeg build failed (see $dir/make.log)"
  printf '%s' "$want" >"$stamp"
}

# ff_score BIN PREFIX OUT_JSON FILTER_GRAPH [extra ffmpeg args...]
# Runs one scoring pass with PREFIX's libraries and checks which libvmaf loaded.
ff_score() {
  local bin="$1" prefix="$2" out="$3" graph="$4" log
  shift 4
  log="$out.run.log"
  rm -f "$out"
  LD_DEBUG=libs LD_LIBRARY_PATH="$(uc_ldpath "$prefix")" timeout 300 "$bin" \
    -hide_banner -nostdin -loglevel info "$@" \
    -f rawvideo -pix_fmt yuv420p -video_size "$SIZE" -i "$DIST_YUV" \
    -f rawvideo -pix_fmt yuv420p -video_size "$SIZE" -i "$REF_YUV" \
    -lavfi "$graph" -an -f null - >"$log" 2>&1 ||
    uc_die "ffmpeg failed (see $log)"
  [ -s "$out" ] || uc_die "ffmpeg wrote no score file $out (see $log)"
  uc_check_loaded "$prefix" "$log"
}

# ff_compare A B LABEL_A LABEL_B: compare and fold the result into STATUS.
ff_compare() {
  local rc=0
  uc_compare "$@" || rc=$?
  case "$rc" in
    0) ;;
    1) STATUS=1 ;;
    *) exit "$UC_EXIT_SETUP" ;;
  esac
}

ff_cpu_leg() {
  local bin="$FF_ROOT/build/ffmpeg" graph out
  out="$WORK/ffmpeg-prefix.json"
  # Upstream input order (libavfilter/vf_libvmaf.c, libvmaf_inputs[]): pad 0
  # "main" is the DISTORTED video, pad 1 "reference" is the reference.
  graph="$(uc_ffmpeg_graph "$out")"
  echo "== ffmpeg: $(LD_LIBRARY_PATH="$(uc_ldpath "$PREFIX")" "$bin" -version | head -n 1)"
  echo "== scoring with --prefix $PREFIX"
  ff_score "$bin" "$PREFIX" "$out" "$graph"
  if [ -n "$REF_PREFIX" ]; then
    echo "== re-running the same ffmpeg binary against --reference-prefix $REF_PREFIX"
    ff_score "$bin" "$REF_PREFIX" "$WORK/ffmpeg-reference.json" \
      "$(uc_ffmpeg_graph "$WORK/ffmpeg-reference.json")"
    ff_compare "$out" "$WORK/ffmpeg-reference.json" "ffmpeg+prefix" "ffmpeg+reference-prefix"
  fi
  if [ "$AGAINST_CLI" -eq 1 ]; then
    echo "== comparing with the vmaf CLI of --prefix"
    uc_cli_scores "$PREFIX" "$WORK/cli-prefix.json"
    ff_compare "$out" "$WORK/cli-prefix.json" "ffmpeg" "vmaf-cli"
  fi
}

# CUDA leg: only when the host can run it; otherwise a stated skip, never a pass.
ff_cuda_skip_reason() {
  [ -f "$PREFIX/include/libvmaf/libvmaf_cuda.h" ] ||
    {
      echo "library under $PREFIX was built without CUDA (no libvmaf_cuda.h)"
      return 0
    }
  command -v nvidia-smi >/dev/null 2>&1 ||
    {
      echo "no nvidia-smi: host has no GPU runtime"
      return 0
    }
  timeout 30 nvidia-smi -L >/dev/null 2>&1 || {
    echo "nvidia-smi lists no device"
    return 0
  }
  PKG_CONFIG_PATH="$(uc_pkgpath "$PREFIX")" pkg-config --exists ffnvcodec 2>/dev/null ||
    {
      echo "ffnvcodec headers not found by pkg-config"
      return 0
    }
  return 0
}

ff_cuda_leg() {
  local reason out graph bin
  reason="$(ff_cuda_skip_reason)"
  if [ -n "$reason" ]; then
    echo "SKIP: libvmaf_cuda filter: $reason"
    return 0
  fi
  ff_build build-cuda --enable-ffnvcodec --enable-cuda \
    --enable-filter=hwupload,hwupload_cuda,libvmaf_cuda,trim,setpts
  grep -q 'define CONFIG_LIBVMAF_CUDA_FILTER 1' "$FF_ROOT/build-cuda/config_components.h" ||
    uc_die "libvmaf_cuda filter not enabled in $FF_ROOT/build-cuda/config_components.h"
  bin="$FF_ROOT/build-cuda/ffmpeg"
  out="$WORK/ffmpeg-cuda-prefix.json"
  local cut="trim=end_frame=$FRAMES,setpts=PTS-STARTPTS"
  graph="[0:v]${cut},hwupload_cuda[d];[1:v]${cut},hwupload_cuda[r];[d][r]libvmaf_cuda=log_fmt=json:log_path=$out"
  echo "== libvmaf_cuda filter with --prefix"
  ff_score "$bin" "$PREFIX" "$out" "$graph" -init_hw_device cuda=cu:0 -filter_hw_device cu
  if [ -n "$REF_PREFIX" ]; then
    ff_score "$bin" "$REF_PREFIX" "$WORK/ffmpeg-cuda-reference.json" \
      "${graph//$out/$WORK/ffmpeg-cuda-reference.json}" -init_hw_device cuda=cu:0 -filter_hw_device cu
    ff_compare "$out" "$WORK/ffmpeg-cuda-reference.json" "cuda+prefix" "cuda+reference-prefix"
  fi
  if [ "$AGAINST_CLI" -eq 1 ]; then
    uc_cli_scores "$PREFIX" "$WORK/cli-cuda.json" --backend cuda
    ff_compare "$out" "$WORK/cli-cuda.json" "ffmpeg-cuda" "vmaf-cli-cuda"
  fi
}

ff_fetch
ff_build build
ff_cpu_leg
if [ "$CUDA" -eq 1 ]; then
  ff_cuda_leg
fi

if [ "$STATUS" -eq 0 ]; then
  echo "PASS: upstream FFmpeg $FF_TAG scores match"
else
  echo "FAIL: upstream FFmpeg $FF_TAG scores differ"
fi
exit "$STATUS"
