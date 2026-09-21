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

# Skip on runners that have the CUDA toolkit installed but no GPU hardware.
# ldconfig finding libcuda.so is insufficient: the stub library passes even
# on CPU-only CI runners (e.g. ubuntu-26.04 with the CUDA 13.4 toolkit stub).
# nvidia-smi -L enumerates actual GPU devices; exit 77 = meson SKIP so the
# test is recorded as skipped rather than failed.
nvidia-smi -L >/dev/null 2>&1 || exit 77

# Fixture size is deliberately 1920x1080 and must stay there.
#
# This test once overran its 10-second Meson budget at 10.09 s, and shrinking
# the fixture to 576x324 was proposed as the fix.  It is not one.  Measured on
# the RTX 4090 workstation, idle, CUDA build, 2 frames:
#
#   vmaf --version                              5 ms   process + link floor
#   576x324   --no_cuda, one invocation        14 ms   CPU scoring only
#   1920x1080 --no_cuda, one invocation        55 ms   CPU scoring only
#   576x324   four invocations, CUDA        ~1020 ms   whole script
#   1920x1080 four invocations, CUDA        ~1000 ms   whole script
#
# ~175 ms of every invocation is fixed CUDA bring-up, so the two fixtures cost
# the same wall time to within noise; the pixel work this constant controls is
# ~160 ms across the whole script.  The overrun is a different effect entirely
# and is bimodal: the first run after a CUDA build/relink was measured at
# 9.70 s and 9.48 s, against 0.95-1.63 s in steady state — a premium paid on
# the driver's module-load path before any pixel is read.  Shrinking the frame
# cannot touch it, and doing so would drop the only 1080p exercise of the CUDA
# dispatch path.  meson.build carries the real fix (a measured timeout plus
# is_parallel) and the full numbers.
WIDTH=1920
HEIGHT=1080

# no gpumask: use cuda
./tools/vmaf \
  --reference /dev/zero \
  --distorted /dev/zero \
  --width "$WIDTH" --height "$HEIGHT" --pixel_format 420 --bitdepth 8 \
  --frame_cnt 2 \
  --gpumask 0

# gpumask: use cpu.
#
# Any NON-ZERO mask disables GPU feature-extractor selection and falls back to
# the CPU implementation (see the `gpumask` docs in libvmaf.h). Upstream writes
# `-1` here, which only ever worked because POSIX strtoul() silently converts
# "-1" to ULONG_MAX; this fork rejects a leading '-' before calling strtoul
# (core/tools/cli_parse.cpp::parse_unsigned) rather than accept a value the
# caller did not mean. `1` expresses the same intent without relying on
# unsigned wraparound. See ADR-1209.
./tools/vmaf \
  --reference /dev/zero \
  --distorted /dev/zero \
  --width "$WIDTH" --height "$HEIGHT" --pixel_format 420 --bitdepth 8 \
  --frame_cnt 2 \
  --gpumask 1

# no gpumask: use cuda for vmaf features, cpu for psnr
./tools/vmaf \
  --reference /dev/zero \
  --distorted /dev/zero \
  --width "$WIDTH" --height "$HEIGHT" --pixel_format 420 --bitdepth 8 \
  --frame_cnt 2 \
  --gpumask 0 \
  --feature psnr \
  --output /dev/stdout

# gpumask: use cpu for vmaf features and psnr (non-zero mask; see above)
./tools/vmaf \
  --reference /dev/zero \
  --distorted /dev/zero \
  --width "$WIDTH" --height "$HEIGHT" --pixel_format 420 --bitdepth 8 \
  --frame_cnt 2 \
  --gpumask 1 \
  --feature psnr
