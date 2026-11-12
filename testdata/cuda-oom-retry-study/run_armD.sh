#!/bin/bash
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2

# Arm D campaign — silent-wait ladder, 6 rounds per device, with an external
# nvidia-smi sampler running alongside (out-of-band view of memory during waits).
# This script runs the shared test binary: apply armD_edit.patch to the build
# worktree and rebuild first, or it measures the as-landed arm instead (see
# SUMMARY.md). Revert the source afterwards.
#
# Usage: run_armD.sh <build-worktree> <results-root>
# The caller puts nvidia-smi and the CUDA runtime DLL directories on PATH;
# the script hard-codes no machine-specific location. Results land in
# <results-root>/armD/ with the same layout as the committed logs.

WORKTREE="${1:?usage: run_armD.sh <build-worktree> <results-root>}"
ROOT="${2:?usage: run_armD.sh <build-worktree> <results-root>}"
EXE="$WORKTREE/build/test/test_cuda_oom_pictures_released.exe"
[ -x "$EXE" ] || {
  echo "test binary missing: $EXE" >&2
  exit 2
}
BASE="$ROOT/armD"
mkdir -p "$BASE/dev0" "$BASE/dev1"

# sampler: both GPUs every 5 s for the whole campaign (nvidia-smi -i: 0=4060, 1=5080)
(
  for i in $(seq 1 400); do
    echo "$(date -u +%H:%M:%S) | 4060: $(nvidia-smi -i 0 --query-gpu=memory.used,memory.free --format=csv,noheader) | 5080: $(nvidia-smi -i 1 --query-gpu=memory.used,memory.free --format=csv,noheader)"
    sleep 5
  done
) >"$BASE/sampler.log" 2>&1 &
SAMPLER_PID=$!
trap 'kill $SAMPLER_PID 2>/dev/null' EXIT

: >"$BASE/timeline.log"
for DEV in 0 1; do
  for i in $(seq -w 1 6); do
    {
      echo "=== armD dev$DEV run$i start $(date -u +%H:%M:%S)"
    } >>"$BASE/timeline.log"
    CUDA_VISIBLE_DEVICES=$DEV "$EXE" \
      >"$BASE/dev$DEV/run-$i.out" 2>"$BASE/dev$DEV/run-$i.err"
    echo "rc=$?" >>"$BASE/dev$DEV/run-$i.err"
    echo "=== armD dev$DEV run$i end   $(date -u +%H:%M:%S)" >>"$BASE/timeline.log"
  done
done
echo "ARM-D-DONE $(date -u +%H:%M:%S)" >>"$BASE/timeline.log"
