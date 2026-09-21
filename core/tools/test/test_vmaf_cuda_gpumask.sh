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
# on CPU-only CI runners (e.g. ubuntu-26.04 with the CUDA 13.3 toolkit stub).
# nvidia-smi -L enumerates actual GPU devices; exit 77 = meson SKIP so the
# test is recorded as skipped rather than failed.
nvidia-smi -L >/dev/null 2>&1 || exit 77

# This is a dispatch smoke test, not a throughput test.  The former 1920x1080
# fixture ran four two-frame scores under one 10-second Meson budget and timed
# out whenever the full suite contended for CPU/GPU time.  576x324 exercises
# the same model, temporal and backend-selection paths without making scheduler
# load part of the pass/fail contract.
WIDTH=576
HEIGHT=324

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
