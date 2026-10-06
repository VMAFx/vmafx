#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# Shared helpers for upstream-ffmpeg-compat.sh and upstream-gstreamer-compat.sh.
# Source it; it defines functions and sets the strict mode both scripts use.
#
# Exit codes used by both scripts: 0 identical, 1 mismatch, 2 setup failure.

# shellcheck disable=SC2034 # every variable below is consumed by the sourcing script
set -euo pipefail
UC_EXIT_SAME=0
UC_EXIT_DIFF=1
UC_EXIT_SETUP=2

UC_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UC_REPO="$(cd "$UC_HERE/../.." && pwd)"

uc_die() {
  echo "ERROR: $*" >&2
  exit "$UC_EXIT_SETUP"
}

uc_log() {
  echo "[upstream-compat] $*" >&2
}

# uc_exact_tag DIR: the tag the checkout in DIR is at, or nothing.
uc_exact_tag() {
  local tag
  if tag="$(git -C "$1" describe --tags --exact-match HEAD 2>/dev/null)"; then
    echo "$tag"
  fi
}

# uc_cfg KEY: value of KEY="..." from build-config.env (dies when absent).
uc_cfg() {
  local key="$1" line
  line="$(grep -m 1 -E "^${key}=" "$UC_REPO/build-config.env")" ||
    uc_die "build-config.env has no ${key}"
  line="${line#*=}"
  line="${line%%#*}"
  line="${line//\"/}"
  echo "${line//[[:space:]]/}"
}

# uc_usage SCRIPT EXTRA_LINES: print the common usage text.
uc_usage() {
  cat <<EOF
usage: $1 --prefix DIR [options]

  --prefix DIR            libvmaf install under test (required)
  --reference-prefix DIR  second libvmaf install; the same consumer binary is
                          run again against it and must score identically
  --against-cli           also compare with DIR/bin/vmaf on the same input
  --frames N              frames to score (default 3)
  --work DIR              cache and scratch directory
                          (default \${TMPDIR:-/tmp}/upstream-consumers)
  --ref FILE --dist FILE  raw yuv420p 8-bit inputs
                          (default: python/test/resource/yuv/src01_hrc00/01_576x324.yuv)
  --size WxH              frame size of the inputs (default 576x324)
$2
exit codes: 0 identical, 1 mismatch, 2 setup failure
EOF
}

# uc_defaults: set option defaults.
uc_defaults() {
  PREFIX=""
  REF_PREFIX=""
  AGAINST_CLI=0
  FRAMES=3
  WORK="${TMPDIR:-/tmp}/upstream-consumers"
  CUDA=0
  SIZE="576x324"
  local yuv="$UC_REPO/python/test/resource/yuv"
  REF_YUV="$yuv/src01_hrc00_576x324.yuv"
  DIST_YUV="$yuv/src01_hrc01_576x324.yuv"
  JOBS="${UPSTREAM_CONSUMER_JOBS:-4}"
}

# uc_parse_common "$@": consume the shared options; sets UC_REST to leftovers.
# Bounded: at most 64 arguments are examined.
uc_parse_common() {
  UC_REST=()
  local i=0
  while [ "$#" -gt 0 ] && [ "$i" -lt 64 ]; do
    i=$((i + 1))
    case "$1" in
      --prefix)
        [ "$#" -ge 2 ] || uc_die "--prefix needs a value"
        PREFIX="$2"
        shift 2
        ;;
      --reference-prefix)
        [ "$#" -ge 2 ] || uc_die "$1 needs a value"
        REF_PREFIX="$2"
        shift 2
        ;;
      --against-cli)
        AGAINST_CLI=1
        shift
        ;;
      --frames)
        [ "$#" -ge 2 ] || uc_die "$1 needs a value"
        FRAMES="$2"
        shift 2
        ;;
      --work)
        [ "$#" -ge 2 ] || uc_die "$1 needs a value"
        WORK="$2"
        shift 2
        ;;
      --ref)
        [ "$#" -ge 2 ] || uc_die "$1 needs a value"
        REF_YUV="$2"
        shift 2
        ;;
      --dist)
        [ "$#" -ge 2 ] || uc_die "$1 needs a value"
        DIST_YUV="$2"
        shift 2
        ;;
      --size)
        [ "$#" -ge 2 ] || uc_die "$1 needs a value"
        SIZE="$2"
        shift 2
        ;;
      --cuda)
        CUDA=1
        shift
        ;;
      *)
        UC_REST+=("$1")
        shift
        ;;
    esac
  done
}

# uc_validate: check the parsed options; makes paths absolute.
uc_validate() {
  [ -n "$PREFIX" ] || uc_die "--prefix is required"
  PREFIX="$(cd "$PREFIX" 2>/dev/null && pwd)" || uc_die "--prefix is not a directory"
  if [ -n "$REF_PREFIX" ]; then
    REF_PREFIX="$(cd "$REF_PREFIX" 2>/dev/null && pwd)" || uc_die "--reference-prefix is not a directory"
  fi
  case "$FRAMES" in '' | *[!0-9]*) uc_die "--frames needs a positive integer" ;; esac
  [ "$FRAMES" -ge 1 ] || uc_die "--frames needs a positive integer"
  case "$SIZE" in [0-9]*x[0-9]*) ;; *) uc_die "--size needs WxH" ;; esac
  SIZE_W="${SIZE%x*}"
  SIZE_H="${SIZE#*x}"
  [ -f "$REF_YUV" ] || uc_die "reference input missing: $REF_YUV"
  [ -f "$DIST_YUV" ] || uc_die "distorted input missing: $DIST_YUV"
  mkdir -p "$WORK" || uc_die "cannot create $WORK"
  WORK="$(cd "$WORK" && pwd)"
  [ -f "$PREFIX/lib/pkgconfig/libvmaf.pc" ] || [ -f "$PREFIX/lib64/pkgconfig/libvmaf.pc" ] ||
    uc_die "no libvmaf.pc under $PREFIX/lib{,64}/pkgconfig"
  UC_SCRIPT_PY="$UC_HERE/upstream_consumer_scores.py"
  [ -f "$UC_SCRIPT_PY" ] || uc_die "missing $UC_SCRIPT_PY"
}

# uc_pkgpath PREFIX: PKG_CONFIG_PATH entries of one install.
uc_pkgpath() {
  echo "$1/lib/pkgconfig:$1/lib64/pkgconfig:$1/share/pkgconfig"
}

# uc_ldpath PREFIX: LD_LIBRARY_PATH entries of one install.
uc_ldpath() {
  echo "$1/lib:$1/lib64"
}

# uc_check_loaded PREFIX LOG: LOG holds LD_DEBUG=libs output of a run. Names the
# libvmaf / libvmafx that executed and fails when it is not from PREFIX.
uc_check_loaded() {
  local prefix="$1" log="$2" found=0 line path
  while IFS= read -r line; do
    path="${line##*calling init: }"
    echo "  loaded: $path"
    case "$path" in
      */libvmaf.so* | */libvmafx.so*)
        case "$path" in
          "$prefix"/*) found=1 ;;
          *) uc_die "loaded $path, which is outside $prefix (silent fallback)" ;;
        esac
        ;;
    esac
  done < <(grep -m 8 -E 'calling init: .*/libvmafx?\.so' "$log")
  [ "$found" -eq 1 ] || uc_die "no libvmaf under $prefix was loaded (see $log)"
}

# uc_compare A B LABEL_A LABEL_B [extra comparator args]: exact-text
# comparison of every frame metric and pooled metric; returns 0/1/2.
uc_compare() {
  local a="$1" b="$2" la="$3" lb="$4"
  shift 4
  python3 "$UC_SCRIPT_PY" "$a" "$b" --label-a "$la" --label-b "$lb" --pooled "$@"
}

# uc_cli_scores PREFIX OUT [extra vmaf args...]: score with the CLI.
uc_cli_scores() {
  local prefix="$1" out="$2"
  shift 2
  [ -x "$prefix/bin/vmaf" ] || uc_die "no $prefix/bin/vmaf for --against-cli"
  LD_LIBRARY_PATH="$(uc_ldpath "$prefix")" timeout 300 "$prefix/bin/vmaf" \
    -r "$REF_YUV" -d "$DIST_YUV" -w "$SIZE_W" -h "$SIZE_H" -p 420 -b 8 \
    --model version=vmaf_v0.6.1 --frame_cnt "$FRAMES" --threads 1 \
    --json -o "$out" "$@" >"$out.log" 2>&1 ||
    uc_die "vmaf CLI failed (see $out.log)"
}
