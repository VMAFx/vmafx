#!/bin/sh -x
set -e

# Graceful skip when no CUDA driver is available (CI / no-GPU machines).
# The C GPU tests use the same pattern: probe, print [skip:...], exit 0.
# Without this guard the first --gpumask 0 invocation loads the SYCL
# runtime (which can block for 30 s trying to enumerate OpenCL devices),
# then falls back to CPU and fails with "problem reading pictures"
# because /dev/zero read on CPU hits a different error path.
if ! ldconfig -p 2>/dev/null | grep -q libcuda; then
  echo "[skip: no CUDA driver (libcuda not found by ldconfig)]"
  exit 0
fi

# no gpumask: use cuda
./tools/vmaf \
  --reference /dev/zero \
  --distorted /dev/zero \
  --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
  --frame_cnt 2 \
  --gpumask 0

# gpumask: use cpu
./tools/vmaf \
  --reference /dev/zero \
  --distorted /dev/zero \
  --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
  --frame_cnt 2 \
  --gpumask -1

# no gpumask: use cuda for vmaf features, cpu for psnr
./tools/vmaf \
  --reference /dev/zero \
  --distorted /dev/zero \
  --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
  --frame_cnt 2 \
  --gpumask 0 \
  --feature psnr \
  --output /dev/stdout

# gpumask: use cpu for vmaf features and psnr
./tools/vmaf \
  --reference /dev/zero \
  --distorted /dev/zero \
  --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
  --frame_cnt 2 \
  --gpumask -1 \
  --feature psnr
