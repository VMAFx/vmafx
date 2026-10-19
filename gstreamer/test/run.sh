#!/bin/sh
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Build the vmafx GStreamer element against the libvmafx of an existing build and run the tests.
#
#   gstreamer/test/run.sh [CPU_BUILD_DIR [CUDA_BUILD_DIR]]
#
# CPU_BUILD_DIR defaults to build-cpu, CUDA_BUILD_DIR to build-cuda (the CUDA tests run when it
# exists and the plug-in was built with GStreamer CUDA). Exit 77 when the GStreamer development
# files are missing.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cpu=${1:-$root/build-cpu}
cuda=${2:-$root/build-cuda}

for pc in gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0; do
  if ! pkg-config --exists "$pc"; then
    echo "skip: $pc development files not found"
    exit 77
  fi
done

jobs=${GST_VMAFX_JOBS:-4}
build() { # <libvmafx build dir> <plug-in build dir>
  if [ ! -f "$2/build.ninja" ]; then
    PKG_CONFIG_PATH="$1/meson-uninstalled" meson setup "$2" "$root/gstreamer" >/dev/null
  fi
  nice -n 10 ninja -C "$2" -j"$jobs" >/dev/null
}

build "$cpu" "$root/build-gst"
export GST_VMAFX_PLUGIN_DIR="$root/build-gst"
export VMAFX_LIB_DIR="$cpu/src"
export VMAF_CLI="$cpu/tools/vmaf"
export VMAFX_YUV_DIR="$root/python/test/resource/yuv"
export VMAFX_BBB_DIR="$root/testdata/bbb"
if [ -d "$cuda" ] && pkg-config --exists gstreamer-cuda-1.0; then
  build "$cuda" "$root/build-gst-cuda"
  export GST_VMAFX_CUDA_PLUGIN_DIR="$root/build-gst-cuda"
  export VMAFX_CUDA_LIB_DIR="$cuda/src"
  export VMAF_CUDA_CLI="$cuda/tools/vmaf"
fi
exec python3 "$here/test_gst_vmafx_parity.py" -v
