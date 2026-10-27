#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Build the sources the `integrations` tidy lane measures (#2860) next to a
# configured and built core build dir:
#
#   <build>/gst        the GStreamer plugin (gstreamer/), CUDA and Vulkan on
#   <build>/ffmpeg     the patched FFmpeg tree of ffmpeg-patches/series.txt,
#                      configured with libvmafx (no libvmaf), CUDA, Vulkan and libdrm, and
#                      only the three objects built from ffmpeg-patches/src/
#   <build>/ffmpeg-objs.log  their compile lines (make V=1)
#
# scripts/ci/gen-integrations-compile-commands.py turns the plugin's database
# and that log into the lane's entries. Both consumers find libvmafx through
# the core build's meson-uninstalled pkg-config files. Usage:
#
#   tidy-integrations-build.sh <core-build-dir> [jobs]
set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "usage: $0 <core-build-dir> [jobs]" >&2
  exit 2
fi
BUILD=$(cd "$1" && pwd)
JOBS=${2:-4}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
UNINSTALLED="$BUILD/meson-uninstalled"
CC_BIN=${CC:-gcc-15}

if [ ! -f "$UNINSTALLED/libvmafx-uninstalled.pc" ]; then
  echo "error: $UNINSTALLED/libvmafx-uninstalled.pc missing; build the core first" >&2
  exit 1
fi
export PKG_CONFIG_PATH="$UNINSTALLED${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# The plugin: both optional bodies are required here, so a missing GStreamer
# CUDA or Vulkan package fails the lane instead of measuring the stubs.
CUDA_ROOT=${CUDA_HOME:-/usr/local/cuda}
rm -rf "$BUILD/gst"
meson setup "$BUILD/gst" "$REPO/gstreamer" -Dcuda=enabled -Dvulkan=enabled \
  -Dtoolkit_root="$CUDA_ROOT" -Db_lto=false
ninja -C "$BUILD/gst" -j "$JOBS"

# The patched FFmpeg tree, replayed exactly as the patch-stack gate replays it.
rm -rf "$BUILD/ffmpeg"
python3 "$REPO/scripts/ci/ffmpeg_patch_stack.py" --check --keep-tree "$BUILD/ffmpeg"
cd "$BUILD/ffmpeg"
# Only libvmafx: the measured filters need nothing else from VMAFx. CUDA comes
# from ffnvcodec; each --enable-* below fails configure when its package is
# missing, so no filter body is measured as a stub. The legacy vmaf_pre filter
# is disabled by name: patch 0002's configure check for it requires libvmaf
# whenever the filter is not disabled, before filter dependencies resolve.
./configure --enable-version3 --enable-libvmafx --disable-filter=vmaf_pre \
  --enable-vulkan --enable-libdrm --enable-ffnvcodec --enable-cuda \
  --disable-doc --disable-indevs --disable-outdevs --cc="$CC_BIN"
for flag in CONFIG_CUDA CONFIG_VULKAN CONFIG_LIBDRM CONFIG_VMAFX_FILTER \
  CONFIG_VMAFX_PRE_FILTER; do
  if ! grep -qE "^#define $flag 1\$" config.h config_components.h; then
    echo "error: FFmpeg configured without $flag" >&2
    exit 1
  fi
done
OBJECTS=(libavfilter/vf_vmafx.o libavfilter/vf_vmafx_pre.o fftools/ffmpeg_dec_hwaccel.o)
make -j "$JOBS" V=1 "${OBJECTS[@]}" >"$BUILD/ffmpeg-objs.log" 2>&1 || {
  tail -n 40 "$BUILD/ffmpeg-objs.log" >&2
  exit 1
}
